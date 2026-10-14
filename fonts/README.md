# fonts/ — 烧进 flash 分区的字体二进制

这个目录里的东西**不是**编译进 app 的，而是**原样写进 `fonts` 数据分区**
（见 `../partitions.csv`），运行时由 `components/font_service` mmap 后用
`cbin_font_create()` 建出 `lv_font_t`。

## 为什么要有它

`font_puhui_16_4` / `font_puhui_20_4`（`78/xiaozhi-fonts`）是**子集化**字体：
只有 **6649 个码位**，其中 CJK 仅 **5258** 个。AI 回复里偶尔会出现字集外的汉字
（例如 U+6B38 欸），LVGL 就报 `glyph dsc. not found` 并画一个占位方框。

`font_noto_qwen_16_4.bin` 是同一个组件里另一个**覆盖大得多**的字体
—— Noto Sans SC/TC/JP/KR 合并后按同一份字表裁剪，**18129 个码位 / CJK 10335**，
包含 U+6B38。它作为 fallback 挂在上面两个字体后面（见 `font_service.c`）。

## 文件

| 文件 | 大小 | sha256 |
|---|---|---|
| `font_noto_qwen_16_4.bin` | 2,051,896 B | `3fab66f1e57215f103a9e9a9d9bf525dfedbe60ebe01875472b3c0b8c7a4e3fe` |

**来源（逐字节相同，未做任何修改）**：
`managed_components/78__xiaozhi-fonts/cbin/font_noto_qwen_16_4.bin`
（组件 `78/xiaozhi-fonts ~1.5.2`）

> **为什么复制一份而不是直接引用 `managed_components/`**：那个目录由组件管理器
> 按需重建，路径不稳定；而**烧进 flash 的确切字节**应当是仓库里能查到的东西。
> 复制完之后 `fonts/` 与上游**不再自动同步** —— 这就是上面那个 sha256 的用途：
> 组件升级后如果想知道有没有变，比一下哈希。

## 复现上游这个文件

（记录在此，方便日后重生成或换字号）

1. `78/xiaozhi-fonts` 的 `generate_fonts.ipynb` cell 3：从
   **`deepseek-ai/DeepSeek-R1-0528` ∪ `Qwen/Qwen3-235B-A22B-Instruct-2507`**
   两个分词器里取出全部字符（共 7415 个），存成 `build/chars.txt`。
2. cell 6：把它并上 ASCII / Latin-1 / xiaozhi 各语言文案，用 fontTools 对
   Noto Sans{SC,TC,JP,KR} 等做子集，再 merge 成 `ttf/noto-qwen.ttf`。
3. cell 10：用 **78 版 lv_font_conv** 生成 cbin：
   ```
   lv_font_conv --force-fast-kern-format --no-compress --no-prefilter \
     --font ttf/noto-qwen.ttf --format cbin --bpp 4 \
     -o cbin/font_noto_qwen_16_4.bin --size 16 -r 0x0-0xfffff
   ```

## ⚠️ 与本工程 sdkconfig 的 ABI 约定（**实测核对过，换文件前必须重核**）

cbin 里的 `lv_font_fmt_txt_glyph_dsc_t` 是**编译期大小决定**的，两边不一致会
**渲染出乱码而不是报错**。核对方法与结论（2026-10-14）：

| 项 | 本工程 sdkconfig | 该 bin | 结论 |
|---|---|---|---|
| `LV_FONT_FMT_TXT_LARGE` | `=y` | **16 字节/条**（实测）| ✅ 一致 |
| `LV_USE_FONT_COMPRESSED` | 未开 | `bitmap_format=0`（PLAIN）| ✅ 一致 |
| bpp | — | 4 | ✅ |

**怎么实测出来的**（不靠猜）：
- `glyph_dsc` 区长度 = 258976 = **16186 × 16**（余 0）；cmap 反推出的最大 glyph id
  是 16184 ⇒ 条目数正好，8 字节解读会多出一倍空槽。
- 按 16 字节读 glyph id 36（'A'）得 `box=11x12 adv_w=10.25px`，
  glyph id 65（'^'）得 `box=9x8 ofs_y=4`（上标位置）—— 都是 16px 字该有的值。
  按 8 字节读，`bitmap_index` 会乱跳（786437、327689 这种），明显不对。

## 分区

`partitions.csv`：`fonts, data, spiffs, 0xD00000, 2M`
（16MB flash 尾部，两个 6MB 的 OTA 槽之后，正好把剩下的空间用掉一部分）。

刷写由 `../CMakeLists.txt` 里的 `esptool_py_flash_to_partition()` 挂到
`idf.py flash` 上，**不需要手工 esptool 命令**。

## 上板实测结果（2026-10-14）

```
I (100) boot:  6 fonts            Unknown data     01 82 00d00000 00200000
I (3019) font: Noto 回退已挂上（分区 fonts 2097152 字节；162 条 cmap，4 bpp）
I (3039) font: 内部堆 172439 → 172019（本组件净占 420 字节）
I (3059) font: 自检通过：U+6B38 普惠体里没有、回退后取到了（16x16，来自Noto 回退）
```

- **`162 条 cmap`** 与用 Python 离线解析这个 bin 得到的数字**逐字一致** ——
  解析脚本和板子上的 `cbin_font_create()` 是两套独立实现，互相印证了偏移算得对。
- **内部堆只多 420 字节**，不是估算：`font_service_init()` 前后各打一次
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 相减。
  （小的原因：162×24=3888 字节的 cmap 描述符数组超过
  `SPIRAM_MALLOC_ALWAYSINTERNAL=512`，被分到 PSRAM 去了。）
- 全程 0 条 `glyph dsc. not found`。
