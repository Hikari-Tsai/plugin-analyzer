# 致謝與第三方授權

Plugin Analyzer 的量測程式碼是本專案自行撰寫的，以 [MIT 授權](LICENSE) 釋出。程式框架和部分內建元件來自下列上游專案；量測方法則依據公開文獻實作。
各套件的授權原文放在 [`third_party_licenses/`](third_party_licenses/)。

## 1. 編進程式的上游程式碼

### JUCE 8.0.6
- 用途：應用程式框架、GUI、FFT（`juce::dsp::FFT`）、VST3/AU 插件載入、`juce::Reverb`
- 授權：**AGPLv3**，或商業授權 **JUCE 8 End User Licence Agreement**（二擇一）
- 來源：https://github.com/juce-framework/JUCE
- 原文：[third_party_licenses/JUCE-LICENSE.md](third_party_licenses/JUCE-LICENSE.md)

> 本專案沒有購買 JUCE 商業授權，所以 JUCE 是以 AGPLv3 授權使用。對外發佈本程式（執行檔或原始碼）時，必須遵守 AGPLv3，包括提供完整原始碼；或者改為取得 JUCE 商業授權。

### JUCE 內建、會被編譯進本程式的第三方套件

以下套件放在 JUCE 的 module 裡。本程式連結了 `juce_audio_utils`、`juce_audio_processors`、`juce_dsp` 及其相依 module，所以它們會被編譯進來。

| 套件 | 版本 | 授權 | 在本程式中的用途 | 原文 |
|---|---|---|---|---|
| Steinberg VST3 SDK | JUCE 8.0.6 內附 | Steinberg VST3 License **或** GPLv3 | 載入 VST3 插件；插件版的 VST3 格式 | [VST3_SDK-LICENSE.txt](third_party_licenses/VST3_SDK-LICENSE.txt) |
| Avid AAX SDK | JUCE 8.0.6 內附 | Avid AAX SDK License **或** GPLv3 | 插件版的 AAX 格式 | [AAX_SDK-LICENSE.txt](third_party_licenses/AAX_SDK-LICENSE.txt) |
| Apple AudioUnitSDK | JUCE 8.0.6 內附 | Apache License 2.0 | 插件版的 AU 格式（macOS） | [AudioUnitSDK-LICENSE.txt](third_party_licenses/AudioUnitSDK-LICENSE.txt) |
| HarfBuzz | 10.1.0 | Old MIT | 文字排版 | [HarfBuzz-COPYING.txt](third_party_licenses/HarfBuzz-COPYING.txt) |
| SheenBidi | JUCE 8.0.6 內附 | Apache License 2.0 | 雙向文字處理 | [SheenBidi-LICENSE.txt](third_party_licenses/SheenBidi-LICENSE.txt) |
| libpng | 1.6.37 | PNG Reference Library License v2 | 截圖存成 PNG | [libpng-LICENSE.txt](third_party_licenses/libpng-LICENSE.txt) |
| IJG libjpeg | 6b | Independent JPEG Group License | JPEG 影像（JUCE 圖形 module） | [IJG-libjpeg-README.txt](third_party_licenses/IJG-libjpeg-README.txt) |
| zlib | 1.2.3 | zlib License | 壓縮（PNG、設定檔） | [zlib-LICENSE.txt](third_party_licenses/zlib-LICENSE.txt) |
| FLAC (libFLAC) | JUCE 8.0.6 內附 | BSD-3-Clause 類 | 音訊格式 module（*） | [FLAC-LICENSE.txt](third_party_licenses/FLAC-LICENSE.txt) |
| Ogg Vorbis | JUCE 8.0.6 內附 | BSD-3-Clause 類 | 音訊格式 module（*） | [OggVorbis-LICENSE.txt](third_party_licenses/OggVorbis-LICENSE.txt) |

（*）FLAC 和 Ogg Vorbis 有被編譯，但本程式沒有讀寫音訊檔，所以在 macOS Release 版裡已經被 linker 移除（執行檔中查不到相關符號）。其他平台或設定下可能會保留，因此仍列在這裡。

**IJG 要求的聲明：** This software is based in part on the work of the Independent JPEG Group.

**AAX 注意事項：** AAX SDK 是 Avid 專有授權與 GPLv3 二擇一。要讓 Pro Tools 正式載入，AAX 插件必須先通過 Avid 開發者計畫，再用 PACE（iLok）的 wraptool 簽署；CI 產出的是未簽署版本。「AAX」與「Pro Tools」是 Avid Technology 的商標。

**VST3 注意事項：** VST3 SDK 是 Steinberg 專有授權與 GPLv3 二擇一。以專有授權發佈前，需要先和 Steinberg 簽署 VST3 授權協議；以 GPLv3 發佈，則整個程式要遵守 GPLv3（可與 JUCE 的 AGPLv3 搭配）。「VST」是 Steinberg Media Technologies GmbH 的商標。

### 確認沒有使用的 JUCE 內建套件
Oboe（Android）、GLEW/Mesa/Khronos（OpenGL）、CHOC/QuickJS（JavaScript）、LV2 SDK（已關閉 LV2 載入）、Box2D、pslextensions。
（AAX SDK 和 AudioUnitSDK 只會編進插件版，獨立 app 不含這兩個。）

### 作業系統元件
- macOS 的 AU 插件載入使用 Apple 系統框架（AudioToolbox、AudioUnit、CoreAudio 等）。這些框架隨作業系統提供，本程式沒有散布它們。

## 2. 參考的方法與文獻（沒有複製程式碼）

以下演算法都依論文或標準的數學定義自行實作：

| 功能 | 依據 |
|---|---|
| 同步指數掃頻、諧波脈衝分離、Hammerstein 模型識別（Hammerstein 分頁） | A. Novak, L. Simon, F. Kadlec, P. Lotton, "Nonlinear System Identification Using Exponential Swept-Sine Signal," *IEEE Trans. Instrumentation and Measurement*, 59(8), 2010.<br>A. Novak, P. Lotton, L. Simon, "Synchronized Swept-Sine: Theory, Application, and Implementation," *J. Audio Eng. Soc.*, 63(10), 2015. |
| 指數掃頻量測脈衝響應與失真的基本概念 | A. Farina, "Simultaneous Measurement of Impulse Response and Distortion with a Swept-Sine Technique," AES 108th Convention, 2000. |
| 殘響能量衰減曲線（Schroeder 反向積分） | M. R. Schroeder, "New Method of Measuring Reverberation Time," *J. Acoust. Soc. Am.*, 37(3), 1965. |
| T20 / T30 / EDT 的定義 | ISO 3382-1（房間聲學參數量測） |
| IMD（60 Hz + 7 kHz，振幅比 4:1） | SMPTE RP 120 互調失真量測方法 |
| 雜訊激發的頻率響應（H1 估計 = 互頻譜 / 自頻譜） | J. S. Bendat, A. G. Piersol, *Random Data: Analysis and Measurement Procedures* |
| 動態範圍（−60 dBFS、A 加權、THD+N + 60 dB）、997 Hz 測試頻率 | AES17（Standard method for digital audio engineering — Measurement of digital audio equipment） |
| A 加權曲線 | IEC 61672-1（頻率加權 A 的解析式） |
| 串音、聲道平衡等類比式規格的定義 | IEC 60268-3（音響設備 — 放大器） |
| True peak / inter-sample peak 的概念（本程式以 FFT 4 倍頻帶限制重建估計，不是 BS.1770 規定的 FIR 濾波器） | ITU-R BS.1770 |
| 群延遲 τ(f) = −dφ/dω | 標準訊號處理定義 |
| Kaiser 視窗（2D 時頻圖） | J. F. Kaiser, "Nonrecursive Digital Filter Design Using the I0-sinh Window Function," IEEE ISCAS, 1974 |
| 內建 Peak EQ 的 biquad 係數 | R. Bristow-Johnson, "Cookbook formulae for audio EQ biquad filter coefficients"（Audio EQ Cookbook） |
| 內建 Simple Reverb | 使用 `juce::Reverb`，JUCE 原始碼註明其「based on the technique and tunings used in FreeVerb」。Freeverb 由 Jezar at Dreampoint 以 public domain 釋出。 |

## 3. 功能規劃參考
各分析分頁的功能範圍，參考了 DDMF Plugindoctor 公開使用手冊描述的量測項目。本專案沒有使用、反編譯或參考 Plugindoctor 的任何程式碼或執行檔；Plugindoctor 是 DDMF（Christian Siedschlag）的產品與商標。

## 4. 只在建置時使用、不隨程式散布的工具
CMake、Ninja、Xcode / Visual Studio、GitHub Actions（`actions/checkout`、`actions/upload-artifact`，MIT 授權）。
