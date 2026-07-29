# box_audio_codec.cc I2S 参数详解

## 环境
- ESP32-S3 + ES8311(输出 DAC) + ES7210(输入 4-mic ADC)
- 双工模式: RX(TDM,4ch) + TX(STD,1ch) 共用 I2S_NUM_0
- 采样率: 输入=输出=16000Hz, 16-bit

---

## 一、I2S 通道配置 (i2s_chan_config_t)
初始化 I2S 物理通道的资源分配。

| 参数 | 值 | 含义 | 影响 |
|------|-----|------|------|
| `id` | `I2S_NUM_0` | I2S 硬件控制器 0 | ESP32-S3 有 2 个 I2S 控制器(0/1) |
| `role` | `I2S_ROLE_MASTER` | 主模式 | ESP32 提供 BCLK/WS 时钟，ES8311/ES7210 从模式 |
| `dma_desc_num` | `8` | DMA 描述符数量 | **音频关键参数** — 太少→buffer underrun→卡顿，太多→浪费内存 |
| `dma_frame_num` | `512` | 每描述符帧数(samples) | **音频关键参数** — 8×512=4096帧缓冲 |
| `auto_clear_after_cb` | `true` | 回调后自动清 DMA buffer | 防止重复播放 |
| `auto_clear_before_cb` | `false` | 回调前不清 buffer | |
| `intr_priority` | `0` | 中断优先级 | 0=默认, 一般不调 |

---

## 二、输出配置 (i2s_std_config_t) — ES8311 DAC
标准 I2S 协议，单声道输出到喇叭。

### 时钟 (clk_cfg)
| 参数 | 值 | 含义 |
|------|-----|------|
| `sample_rate_hz` | `16000` | 输出采样率 |
| `clk_src` | `I2S_CLK_SRC_DEFAULT` | PLL_F160M 时钟源 |
| `mclk_multiple` | `I2S_MCLK_MULTIPLE_256` | MCLK = 256 × Fs = 4.096MHz |

### 时隙 (slot_cfg)
| 参数 | 值 | 含义 |
|------|-----|------|
| `data_bit_width` | `I2S_DATA_BIT_WIDTH_16BIT` | 16-bit 音频数据 |
| `slot_bit_width` | `I2S_SLOT_BIT_WIDTH_AUTO` | 自动=16bit |
| `slot_mode` | `I2S_SLOT_MODE_STEREO` | 左右双声道 |
| `slot_mask` | `I2S_STD_SLOT_BOTH` | 左右声道都发送(兼容单声道) |
| `ws_width` | `I2S_DATA_BIT_WIDTH_16BIT` | WS 信号宽度 |
| `ws_pol` | `false` | WS=0→左声道 |
| `bit_shift` | `true` | SCK 上升沿采样 |
| `left_align` | `true` | 左对齐格式 |
| `big_endian` | `false` | 小端 |
| `bit_order_lsb` | `false` | MSB 先发 |

### GPIO
| 参数 | 来源 | 说明 |
|------|------|------|
| `mclk` | config.h `AUDIO_I2S_GPIO_MCLK` | 主时钟 |
| `bclk` | config.h `AUDIO_I2S_GPIO_BCLK` | 位时钟 |
| `ws` | config.h `AUDIO_I2S_GPIO_WS` | 字选/左右声道 |
| `dout` | config.h `AUDIO_I2S_GPIO_DOUT` | 数据输出→ES8311 |
| `din` | `I2S_GPIO_UNUSED` | 标准模式不用输入 |

---

## 三、输入配置 (i2s_tdm_config_t) — ES7210 4-mic ADC
TDM(时分复用)协议，4 路麦克风输入用于 AEC 回声消除。

### 时钟 (clk_cfg)
| 参数 | 值 | 含义 |
|------|-----|------|
| `sample_rate_hz` | `16000` | 输入采样率(每通道) |
| `clk_src` | `I2S_CLK_SRC_DEFAULT` | |
| `mclk_multiple` | `I2S_MCLK_MULTIPLE_256` | |
| **`bclk_div`** | **`8`** | **BCLK 分频** — TDM 需要更高 BCLK 传 4 通道数据 |

### 时隙 (slot_cfg)
| 参数 | 值 | 含义 |
|------|-----|------|
| `data_bit_width` | `I2S_DATA_BIT_WIDTH_16BIT` | 16-bit |
| `slot_bit_width` | `I2S_SLOT_BIT_WIDTH_AUTO` | |
| `slot_mode` | `I2S_SLOT_MODE_STEREO` | |
| **`slot_mask`** | `SLOT0\|SLOT1\|SLOT2\|SLOT3` | **4通道** — MIC1/3/2/4 物理顺序 |
| `ws_width` | `I2S_TDM_AUTO_WS_WIDTH` | |
| `left_align` | `false` | 标准 I2S 对齐 |
| `bit_shift` | `true` | |
| `total_slot` | `I2S_TDM_AUTO_SLOT_NUM` | 自动=4 |

### GPIO
| 参数 | 来源 | 说明 |
|------|------|------|
| `mclk/bclk/ws` | 同输出 | 共用时钟线 |
| `dout` | `I2S_GPIO_UNUSED` | TDM 模式不用输出 |
| `din` | config.h `AUDIO_I2S_GPIO_DIN` | 数据输入←ES7210 |

---

## 四、旧 build vs 新编译的差异

旧 `box_audio_codec.cc.obj` = 113080 bytes（声音正常）
新编译 `box_audio_codec.cc.obj` = 117637 bytes（声音发抖）
差异 = +4557 bytes

源码完全一样→差异来自 **IDF v5.5.4-dirty vs v5.5.4** 的头文件(结构体定义/宏展开)。
可能影响的结构体: `i2s_chan_config_t`, `i2s_std_config_t`, `i2s_tdm_config_t` 的成员布局。

**修改此文件时的注意事项:**
- 上面的表里每个参数都标了确切值和含义
- 如果要改参数，从上表找到对应的值直接改
- 改完后编译测试，如果声音正常就 OK
- 如果改完后声音又发抖→说明IDF头文件差异仍然存在→需要进一步排查
