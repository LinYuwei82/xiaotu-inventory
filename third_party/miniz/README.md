# miniz（内嵌源码）

- 上游：https://github.com/richgel999/miniz ，版本 `3.0.2`。
- `miniz.c` / `miniz.h` 由上游 `amalgamate.sh` 的拼接步骤生成（单文件风格，便于内嵌）。
- `miniz_export.h` 是上游 CMake 生成的头文件，这里提供等价的空导出宏。
- 本仓库只使用解压（inflate）：编译时定义 `MINIZ_NO_DEFLATE_APIS`、
  `MINIZ_NO_ARCHIVE_APIS`、`MINIZ_NO_STDIO`、`MINIZ_NO_TIME`，不编入压缩与 ZIP 归档 API。
