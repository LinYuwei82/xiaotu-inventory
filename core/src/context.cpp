#include "xiaotu/context.h"

#include "xiaotu/crud.h"
#include "xiaotu/db.h"

namespace xiaotu {
namespace {
thread_local RequestContext t_context;
}

RequestContext& current_context() { return t_context; }

std::string current_user() { return t_context.user; }

AuditStamp audit_stamp() {
  AuditStamp stamp;
  stamp.at = now_text();
  stamp.by = t_context.user.empty() ? "system" : t_context.user;
  return stamp;
}

}  // namespace xiaotu
