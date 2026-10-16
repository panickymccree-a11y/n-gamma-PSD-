# PSD 新增功能

新增独立 UDP 尾部电荷比 PSD 调试模式，详细说明见同级上层 PSD_README.md。构建输出为 build-psd，使用 50T 固件的 type 7 事件。

# MCA Host

Qt 6/C++20 desktop application for the MCA FPGA.

Network defaults:

- FPGA IPv4 address: `192.168.1.10`
- MCA2 data/response receive port: `9000`
- MCF2 control port: `9001`

MCA2 uses a 20-byte network-byte-order header and typed 12-byte channel,
52-byte module or 112-byte coincidence records. MCF2 uses fixed 20-byte
requests with transaction IDs, ACK/NACK responses, shadow writes and atomic
commit.

The product UI starts UDP acquisition directly. The simulation source remains
available only through the test-facing `AcquisitionController` interface.
Existing MCAL v3 List files are read-only compatible; new single-channel runs
can optionally write `MCA_EVENT_TXT_V1` UTF-8, TAB-delimited event files.

The event TXT columns are:

```text
event_index timestamp_ticks time_ns channel peak_raw peak_channel integral_raw pileup overflow polarity
```

## Waveform mode

Selecting `波形模式` in the acquisition-mode selector switches the centre
stack to an independent oscilloscope-style page. It shares the threshold,
polarity and baseline configuration with the spectrum mode and uses the MCA2
type-6 fragment protocol. `../Docs/PSD_protocol.md` describes the separate
PSD type-7 extension.

The page provides the shared threshold and polarity, a continuous/single
trigger selector, start/stop, a dark 10 x 8 division graticule with the
trigger marker at T=0, the latched baseline and the real `B +/- H` threshold
levels, a status line (filling, waiting, capturing, uploading, complete,
stopped, reception incomplete) with complete/discarded counters, and the
Time/div, ADC/div, horizontal and vertical position, full-view, Y auto scale
and CSV export tools.

Export writes exactly 12500 rows with the columns
`sample_index,time_ns,adc_code,overrange`, keeps the raw ADC codes (no
baseline subtraction, no display scaling) and commits the file atomically.

A device that does not report format capability bit 5 disables the waveform
entry point and explains why; no new register writes are sent to it.

Build and test (configure local Qt/MSVC paths first; see `../Docs/BUILD.md`):

```powershell
.\build_and_test.cmd
```

The script configures and builds the Release application, deploys Qt, and then
runs the tests. `windeployqt --compiler-runtime` copies the Qt plugins and DLLs
and places Microsoft's supported `vc_redist.x64.exe` beside the application.
The resulting `build`
directory can be copied to another 64-bit Windows machine as one deployment
directory; keep its DLL and plugin subdirectories beside the executable. On a
machine without the Microsoft Visual C++ runtime, run `install_runtime.cmd`
(or the bundled `vc_redist.x64.exe`) once before starting `mca_studio.exe`.
Individual VC runtime DLLs are intentionally not copied app-local because a
partial CRT/UCRT mixture can terminate Qt during process startup.

The MSVC workload must include a Windows 10/11 SDK (`rc.exe`, UCRT headers,
and Windows import libraries).
