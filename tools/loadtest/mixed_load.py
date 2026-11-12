#!/usr/bin/env python3
"""小兔进销存并发压测：登录 + 单据保存/审核 + 报表查询混合负载。

只依赖 Python 标准库，直接对着已经启动的网络版服务跑：

    XIAOTU_PASSWORD='admin 口令' python3 tools/loadtest/mixed_load.py \
        --base http://127.0.0.1:8080 --users 16 --duration 20

脚本会：
  1. 用管理员建好压测用的商品、供应商、客户与 N 个 operator 账号（幂等，重复跑会复用）；
  2. 给每个商品入一笔大库存，保证出库单不会因为缺货失败；
  3. N 个线程各自登录，按固定权重交替执行：采购入库、销售出库、草稿+审核、报表、单据列表；
  4. 统计每类请求的 p50/p90/p99/max 与错误率；
  5. 校验单据号唯一、库存余额与流水一致、库存数量与「初始 + 入 - 出」吻合。

带上 --db <app.db> 才会做第 5 步的数据库核对（需要能只读打开数据库）。

退出码：0 = 全部通过；1 = 有错误或核对不通过。
"""

import argparse
import http.client
import json
import os
import sqlite3
import sys
import threading
import time
from datetime import date
from urllib.parse import urlsplit

ITEMS = 4
SEED_QTY = 200000
SEQ_OPS = ["in", "out", "in", "out", "draft", "report", "list"]


class ApiError(RuntimeError):
    def __init__(self, status, code, message):
        super().__init__(f"{status} {code}: {message}")
        self.status = status
        self.code = code


class Client:
    """单线程使用的 HTTP 客户端，保留 Cookie 与 CSRF。"""

    def __init__(self, base, timeout=60):
        parts = urlsplit(base)
        self.host = parts.hostname
        self.port = parts.port or 80
        self.timeout = timeout
        self.conn = None
        self.cookies = {}
        self.csrf = None

    def _connect(self):
        if self.conn is None:
            self.conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)

    def close(self):
        if self.conn is not None:
            self.conn.close()
            self.conn = None

    def request(self, method, path, body=None):
        headers = {"Content-Type": "application/json"}
        if self.cookies:
            headers["Cookie"] = "; ".join(f"{key}={value}" for key, value in self.cookies.items())
        if self.csrf and method != "GET":
            headers["X-CSRF-Token"] = self.csrf
        payload = json.dumps(body) if body is not None else None
        try:
            self._connect()
            self.conn.request(method, path, body=payload, headers=headers)
            response = self.conn.getresponse()
            raw = response.read()
        except (http.client.HTTPException, OSError):
            # 连接被服务端回收时重试一次，避免把连接复用问题当成业务错误。
            self.close()
            self._connect()
            self.conn.request(method, path, body=payload, headers=headers)
            response = self.conn.getresponse()
            raw = response.read()
        connection_close = False
        for header, value in response.getheaders():
            if header.lower() == "set-cookie":
                pair = value.split(";", 1)[0]
                if "=" in pair:
                    name, cookie_value = pair.split("=", 1)
                    if cookie_value:
                        self.cookies[name.strip()] = cookie_value.strip()
                    else:
                        self.cookies.pop(name.strip(), None)
            elif header.lower() == "connection" and value.strip().lower() == "close":
                connection_close = True
        if connection_close:
            # 服务端关闭了 keep-alive，下次请求重新建连，避免在已关闭的套接字上重试。
            self.close()
        return response.status, raw

    def json(self, method, path, body=None):
        status, raw = self.request(method, path, body)
        text = raw.decode("utf-8", "replace")
        try:
            data = json.loads(text) if text else None
        except json.JSONDecodeError as error:
            raise ApiError(status, "BAD_JSON", f"{path} 返回非 JSON：{error}") from error
        if status >= 400:
            code = data.get("code", "HTTP_ERROR") if isinstance(data, dict) else "HTTP_ERROR"
            message = data.get("message", text[:200]) if isinstance(data, dict) else text[:200]
            raise ApiError(status, code, message)
        return data


class Collector:
    def __init__(self):
        self.lock = threading.Lock()
        self.latencies = {}
        self.errors = []
        self.created = []
        self.stock_delta = {}

    def record(self, category, elapsed_ms):
        with self.lock:
            self.latencies.setdefault(category, []).append(elapsed_ms)

    def record_error(self, category, message):
        with self.lock:
            self.errors.append((category, message))

    def record_doc(self, doc_type, item_id, delta):
        with self.lock:
            self.created.append((doc_type, item_id))
            self.stock_delta[item_id] = self.stock_delta.get(item_id, 0) + delta


def balance_by_item(client):
    """按商品汇总库存数量（跨仓库相加），用于核对压测前后变化。"""
    totals = {}
    for row in client.json("GET", "/api/reports/stock-balance"):
        item_id = row["item_id"]
        totals[item_id] = totals.get(item_id, 0) + row["qty"]
    return totals


def login(client, username, password):
    data = client.json("POST", "/api/login", {"username": username, "password": password})
    client.csrf = data["csrf"]
    return data


def timed(collector, category, func):
    started = time.perf_counter()
    try:
        result = func()
        collector.record(category, (time.perf_counter() - started) * 1000)
        return result
    except ApiError as error:
        collector.record(category, (time.perf_counter() - started) * 1000)
        collector.record_error(category, str(error))
        return None
    except (OSError, http.client.HTTPException) as error:
        collector.record(category, (time.perf_counter() - started) * 1000)
        collector.record_error(category, f"连接错误: {error}")
        return None


def ensure_master_data(admin, collector):
    warehouses = admin.json("GET", "/api/warehouses")
    active = [row for row in warehouses if row.get("status")]
    if not active:
        raise SystemExit("没有可用仓库，无法压测")
    warehouse_id = active[0]["id"]

    items = {row["code"]: row["id"] for row in admin.json("GET", "/api/items")}
    for index in range(1, ITEMS + 1):
        code = f"LOAD-ITEM-{index}"
        if code not in items:
            created = admin.json(
                "POST",
                "/api/items",
                {"code": code, "name": f"压测商品 {index}", "unit_id": 1, "unit": "件"},
            )
            items[code] = created["id"]

    suppliers = {row["code"]: row["id"] for row in admin.json("GET", "/api/suppliers")}
    if "LOAD-SUP" not in suppliers:
        suppliers["LOAD-SUP"] = admin.json(
            "POST", "/api/suppliers", {"code": "LOAD-SUP", "name": "压测供应商"}
        )["id"]

    customers = {row["code"]: row["id"] for row in admin.json("GET", "/api/customers")}
    if "LOAD-CUS" not in customers:
        customers["LOAD-CUS"] = admin.json(
            "POST", "/api/customers", {"code": "LOAD-CUS", "name": "压测客户"}
        )["id"]

    item_ids = [items[f"LOAD-ITEM-{index}"] for index in range(1, ITEMS + 1)]
    # 每个商品先入一大笔库存，压测期间的销售出库不会因为缺货失败。
    for item_id in item_ids:
        admin.json(
            "POST",
            "/api/documents",
            {
                "doc_type": "PUR_IN",
                "warehouse_id": warehouse_id,
                "partner_id": suppliers["LOAD-SUP"],
                "lines": [{"item_id": item_id, "qty": SEED_QTY, "price_cents": 100}],
            },
        )
    baseline = balance_by_item(admin)
    print(f"准备完成：仓库 {warehouse_id}，商品 {item_ids}，每项入库 {SEED_QTY}")
    return warehouse_id, item_ids, suppliers["LOAD-SUP"], customers["LOAD-CUS"], baseline


def ensure_users(admin, count, password):
    existing = {row["username"] for row in admin.json("GET", "/api/users")}
    names = []
    for index in range(1, count + 1):
        username = f"load{index:02d}"
        names.append(username)
        if username in existing:
            continue
        admin.json(
            "POST",
            "/api/users",
            {
                "username": username,
                "display_name": f"压测 {index:02d}",
                "role": "operator",
                "password": password,
            },
        )
    print(f"压测账号：{names}")
    return names


def worker(
    index,
    args,
    collector,
    deadline_holder,
    login_barrier,
    go,
    warehouse_id,
    item_ids,
    supplier_id,
    customer_id,
):
    client = Client(args.base)
    timed(collector, "login", lambda: login(client, args.usernames[index], args.password))
    # 所有账号登录完成后再一起开跑，避免登录风暴混进单据/报表的稳态延迟。
    login_barrier.wait()
    go.wait()

    counter = 0
    seq = SEQ_OPS[index % len(SEQ_OPS):] + SEQ_OPS[: index % len(SEQ_OPS)]
    while time.time() < deadline_holder["deadline"]:
        op = seq[counter % len(seq)]
        item_id = item_ids[(counter + index) % len(item_ids)]
        counter += 1

        if op == "in":
            result = timed(
                collector,
                "doc_approve_in",
                lambda: client.json(
                    "POST",
                    "/api/documents",
                    {
                        "doc_type": "PUR_IN",
                        "warehouse_id": warehouse_id,
                        "partner_id": supplier_id,
                        "lines": [{"item_id": item_id, "qty": 1, "price_cents": 100}],
                    },
                ),
            )
            if result:
                collector.record_doc("PUR_IN", item_id, 1)
        elif op == "out":
            result = timed(
                collector,
                "doc_approve_out",
                lambda: client.json(
                    "POST",
                    "/api/documents",
                    {
                        "doc_type": "SAL_OUT",
                        "warehouse_id": warehouse_id,
                        "partner_id": customer_id,
                        "lines": [{"item_id": item_id, "qty": 1, "price_cents": 150}],
                    },
                ),
            )
            if result:
                collector.record_doc("SAL_OUT", item_id, -1)
        elif op == "draft":
            draft = timed(
                collector,
                "doc_draft",
                lambda: client.json(
                    "POST",
                    "/api/documents",
                    {
                        "doc_type": "PUR_IN",
                        "warehouse_id": warehouse_id,
                        "partner_id": supplier_id,
                        "save_as_draft": True,
                        "lines": [{"item_id": item_id, "qty": 1, "price_cents": 100}],
                    },
                ),
            )
            if draft:
                approved = timed(
                    collector,
                    "doc_approve",
                    lambda: client.json("POST", f"/api/documents/{draft['id']}/approve"),
                )
                if approved is not None:
                    collector.record_doc("PUR_IN", item_id, 1)
        elif op == "report":
            timed(collector, "report", lambda: client.json("GET", "/api/reports/stock-balance"))
        else:
            timed(
                collector,
                "doc_list",
                lambda: client.json("GET", "/api/documents?doc_type=PUR_IN"),
            )
        time.sleep(0.005)
    client.close()


def percentiles(values):
    if not values:
        return 0.0, 0.0, 0.0, 0.0
    ordered = sorted(values)

    def pick(ratio):
        return ordered[min(len(ordered) - 1, int(round((len(ordered) - 1) * ratio)))]

    return pick(0.5), pick(0.9), pick(0.99), ordered[-1]


def verify_api_stock(client, baseline, item_ids, delta):
    print("\n=== 库存核对（接口） ===")
    failures = []
    actual = balance_by_item(client)
    for item_id in item_ids:
        expected = baseline.get(item_id, 0) + delta.get(item_id, 0)
        got = actual.get(item_id, 0)
        print(f"商品 {item_id}: 期望 {expected}，实际 {got}")
        if got != expected:
            failures.append(f"商品 {item_id} 库存数量不符（期望 {expected}，实际 {got}）")
    return failures


def verify_database(path, warehouse_id, item_ids, baseline, delta):
    print("\n=== 数据库核对 ===")
    conn = sqlite3.connect(path, timeout=10)
    failures = []
    try:
        duplicate = conn.execute(
            "SELECT COUNT(*) FROM (SELECT doc_no FROM doc_header GROUP BY doc_no HAVING COUNT(*) > 1)"
        ).fetchone()[0]
        print(f"单据号重复: {duplicate}")
        if duplicate:
            failures.append("存在重复单据号")

        mismatch = conn.execute(
            "SELECT COUNT(*) FROM stock_balance b WHERE b.qty <> ("
            " SELECT COALESCE(SUM(qty), 0) FROM stock_ledger l"
            " WHERE l.warehouse_id = b.warehouse_id AND l.item_id = b.item_id)"
        ).fetchone()[0]
        print(f"库存余额与流水不一致的行数: {mismatch}")
        if mismatch:
            failures.append("库存余额与流水不一致")

        today = date.today().isoformat()
        for prefix, doc_type in (("CGRK", "PUR_IN"), ("XSCK", "SAL_OUT")):
            period = today.replace("-", "")
            row = conn.execute(
                "SELECT next_val FROM doc_seq WHERE doc_type = ? AND period_key = ?",
                (prefix, period),
            ).fetchone()
            docs = conn.execute(
                "SELECT COUNT(*) FROM doc_header WHERE doc_type = ? AND doc_date = ?",
                (doc_type, today),
            ).fetchone()[0]
            # doc_seq.next_val 记录的是已分配出去的最大序号，不是「下一个」。
            allocated = row[0] if row else 0
            print(f"{doc_type} 取号: 已分配 {allocated}，实际单据 {docs}")
            if allocated != docs:
                failures.append(f"{doc_type} 取号数量与单据数量不一致")

        for item_id in item_ids:
            actual = conn.execute(
                "SELECT qty FROM stock_balance WHERE warehouse_id = ? AND item_id = ?",
                (warehouse_id, item_id),
            ).fetchone()
            actual_qty = actual[0] if actual else 0
            expected = baseline.get(item_id, 0) + delta.get(item_id, 0)
            print(f"商品 {item_id}: 期望 {expected}，实际 {actual_qty}")
            if actual_qty != expected:
                failures.append(f"商品 {item_id} 库存数量不符（期望 {expected}，实际 {actual_qty}）")
    finally:
        conn.close()
    return failures


def main():
    parser = argparse.ArgumentParser(description="小兔进销存并发压测")
    parser.add_argument("--base", default="http://127.0.0.1:8080")
    parser.add_argument("--admin-user", default="admin")
    parser.add_argument("--admin-password", default=os.environ.get("XIAOTU_PASSWORD", ""))
    parser.add_argument("--users", type=int, default=12, help="并发用户数（10–30）")
    parser.add_argument("--duration", type=float, default=20.0, help="混合负载持续秒数")
    parser.add_argument("--password", default="LoadPassw0rd!", help="压测账号口令")
    parser.add_argument("--db", help="app.db 路径，提供后做数据库核对")
    args = parser.parse_args()

    if not args.admin_password:
        parser.error("缺少管理员口令：用 --admin-password 或环境变量 XIAOTU_PASSWORD")
    if not 1 <= args.users <= 64:
        parser.error("--users 应在 1–64 之间")

    collector = Collector()
    admin = Client(args.base)
    login(admin, args.admin_user, args.admin_password)
    warehouse_id, item_ids, supplier_id, customer_id, baseline = ensure_master_data(admin, collector)
    args.usernames = ensure_users(admin, args.users, args.password)
    admin.close()

    deadline_holder = {"deadline": float("inf")}
    login_barrier = threading.Barrier(args.users + 1)
    go = threading.Event()
    threads = [
        threading.Thread(
            target=worker,
            args=(
                index,
                args,
                collector,
                deadline_holder,
                login_barrier,
                go,
                warehouse_id,
                item_ids,
                supplier_id,
                customer_id,
            ),
            daemon=True,
        )
        for index in range(args.users)
    ]
    for thread in threads:
        thread.start()
    # 等所有线程登录完成，再统一开始计时窗口。
    login_barrier.wait()
    deadline_holder["deadline"] = time.time() + args.duration
    started = time.time()
    go.set()
    for thread in threads:
        thread.join()
    elapsed = time.time() - started

    rows = []
    total = 0
    errors = 0
    for category in sorted(collector.latencies):
        values = collector.latencies[category]
        count = len(values)
        total += count
        p50, p90, p99, worst = percentiles(values)
        rows.append((category, count, p50, p90, p99, worst))

    print("\n=== 压测结果 ===")
    print(f"并发用户: {args.users}  时长: {elapsed:.1f}s  总请求: {total}  吞吐: {total / elapsed:.1f} req/s")
    print(f"{'类别':<18}{'次数':>8}{'p50(ms)':>10}{'p90(ms)':>10}{'p99(ms)':>10}{'max(ms)':>10}")
    for category, count, p50, p90, p99, worst in rows:
        print(f"{category:<18}{count:>8}{p50:>10.1f}{p90:>10.1f}{p99:>10.1f}{worst:>10.1f}")

    print(f"\n错误总数: {len(collector.errors)}")
    for category, message in collector.errors[:20]:
        print(f"  [{category}] {message}")
    if len(collector.errors) > 20:
        print(f"  ... 其余 {len(collector.errors) - 20} 条省略")

    failures = []
    if collector.errors:
        failures.append("存在请求错误")

    doc_numbers = len(collector.created)
    print(f"\n成功审核单据: {doc_numbers}")

    checker = Client(args.base)
    login(checker, args.admin_user, args.admin_password)
    failures.extend(verify_api_stock(checker, baseline, item_ids, collector.stock_delta))
    checker.close()
    if args.db:
        failures.extend(
            verify_database(args.db, warehouse_id, item_ids, baseline, collector.stock_delta)
        )
    else:
        print("未提供 --db，跳过单据号与库存流水的数据库核对")

    if failures:
        print("\n结论: 未通过")
        for item in failures:
            print(f"  - {item}")
        return 1
    print("\n结论: 通过（无请求错误，单据号与库存核对一致）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
