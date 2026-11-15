# Audio output — plan and results

**Status, 2026-11-04: A0–A3 implemented with host, target and device results
recorded below; A4 and the remaining sound-quality, sample-rate and sleep
checks are PENDING.** The user chose the speaker only
(no microphone in this milestone) and an application API from the start
(`rep_audio_v1`). Audio is named in the specification only as a system worker
(§5.2) and in the hardware list (§3); this page is the design record. Labels
follow AGENTS.md: **fact**, **proposal**, **assumption**, **experiment**.

## Hardware facts (Rev1.1 schematic, hash `d4333dcc…9b6d`, drawing only)

| Part | Fact |
|---|---|
| Codec | ES8311 (U9), I²C address **0x18** (the drawing labels it; CE is pulled low by R35). SCL/SDA on the shared bus GPIO10/11 with 2.2 kΩ pull-ups (R44, R45) |
| I²S | MCLK GPIO2, BCLK GPIO48, LRCK GPIO38, data to the codec GPIO47, data from the codec GPIO39 (not used here); the IO table agrees with `docs/hardware-audit.md` |
| Analog supply | A3V3 from an RT9193-33 LDO (U8) fed from VCC and enabled by `PWR_N_EN`, the same enable as the 3V3 buck |
| Amplifier | NS4150B class-D (U10): differential input from the codec OUTP/OUTN through 100 nF and 150 kΩ; **CTRL = `PA_CTRL` = GPIO9** with a 10 kΩ pull-down (R42), so the amplifier is off until driven high; supply is the VCC rail |
| Speaker | Output VoP/VoN goes to a 2-pin header **H1** (0 Ω links R39/R43, filter capacitors not fitted). No speaker is drawn on the board: one must be connected to H1 |
| Strapping | GPIO2, 9, 38, 39, 47, 48 are not strapping pins on the ESP32-S3 |

## Decisions (proposals until the user objects; defaults in force)

- **Codec driver:** Espressif `esp_codec_dev` (the library the Waveshare board
  support uses) instead of a register-level driver of our own (AGENTS.md: look
  for existing facilities first). Its compile on the pinned IDF 6.1 was checked in
  a scratch project: version **1.6.2** with the ES8311 driver, the I²C control
  interface on an existing `i2c_master_bus_handle_t` and the I²S data interface
  built without warnings (compilation only, not a hardware result).
- **One I²C master:** the codec shares the board's managed bus; no second master
  and no legacy I²C or I²S driver.
- **Board boundary (2026-11-15):** the board support creates the I²S channel, the
  codec and the amplifier control (`roundie_board_speaker_start`, an
  `esp_codec_dev` output device; pins and addresses in the board's
  `board_pins.h`). `roundie_audio` keeps the task, tones, the stream ring, volume
  and the power interplay, and runs only on a board with
  `ROUNDIE_BOARD_CAP_SPEAKER` ([board-support.md](board-support.md)). The
  code moved unchanged; the A1–A3 results below were measured before the move.
- **Playback,** 16-bit stereo PCM, sample rates fixed per stream. Recording is
  not an application feature. On a board with `ROUNDIE_BOARD_CAP_MICROPHONE`
  (the 1.75C's ES7210) the board also hands over an input device on the same
  I²S port, opened full duplex. Only the development RA1 command `MIC <ms>`
  uses it: a worker owns the codec for the recording (100..5000 ms, 30 dB
  gain, MIC1 and MIC2) and logs the peak and RMS of each channel. Device
  results: [hardware-audit-1.75c.md](hardware-audit-1.75c.md#device-results-2026-11-13-and-2026-11-14).
- **A system audio task** feeds the I²S DMA; applications never own a task or the
  codec. An application writes PCM into a bounded ring through `rep_audio_v1`
  (gate A3); the ring and every handle belong to the application instance and
  are released on unload before the ELF is freed.
- **Volume is capped by the system** (`ROUNDIE_AUDIO_MAX_PERCENT`, 50 as a first
  value). The speaker is unknown and the amplifier runs from the battery rail;
  the cap is an assumption to be replaced by a measurement.
- **The amplifier is on only while a stream is open** (`PA_CTRL` high), and light
  sleep is refused while audio plays.

## Gates

| Gate | Content | Acceptance |
|---|---|---|
| A0 | This page: facts, decisions, compile check | Reviewed |
| A1 | `roundie_audio` component: I²S, ES8311 and amplifier bring-up; a tone from a development command | Host test of the tone generator; device: codec ACKs at 0x18 and reports its ID, no I²S or I²C error, touch and gauge reads keep working during a tone; **audible output PENDING the user** (needs a speaker on H1) |
| A2 | Stream engine: ring, volume cap, underrun policy, stop on request; power interplay | Host tests; device: no underruns at the chosen buffer, sleep refused while playing |
| A3 | `rep_audio_v1` for applications: ABI, validator allowlist, per-instance resources, example application | Host and target checks; device: playback from an application, stop on unload with no call into unloaded code (passed 2026-11-04, see below) |
| A4 | Validation | Repeated start and stop, unload while playing, load and latency measurements |

## Pending experiments

| Question | Method | Acceptance |
|---|---|---|
| Is a speaker connected, and its rating? | Ask the user; read the markings | Known before any loud test |
| Safe maximum volume | Step the codec volume from low with the speaker attached | Documented value, no clipping or audible distortion |
| I²S and the codec across light sleep | Play, sleep, wake, play again | Works or is re-initialised after wake |
| Timing of start and stop (pops) | Listen; scope if needed | No audible pop, or a documented mute sequence |

## Gate A1 results, 2026-11-04 (firmware 0.3.44)

**Implemented.** `components/roundie_audio`: `tone.c` (integer sine generator, 5 ms
fades), `audio.c` (the system task, I2S transmit channel, ES8311 and the amplifier
through `esp_codec_dev` 1.6.2), `roundie_board_i2c_bus` so the codec joins the board's
managed master. RA1 (`tools/roundie_audio.py`, development only): `status`, `tone`,
`stop`, `regs` (the codec registers 0x00..0x45 as hex) and `melody` (the theme of
Pachelbel's Canon, one voice, about 19 s, so the output can be judged by ear).

**Verified**

| Evidence | Result |
|---|---|
| Host | Tone generator: out-of-range requests, frequency (880 zero crossings in one second at 440 Hz), peak within the request, chunk size does not change the samples, fades, clamping. Full suite: 89 tests OK |
| Target build | 0.3.42 to 0.3.44, no new warnings; the audio component adds about 44 KB |
| Device, codec identity | The ES8311 answers at 0x18; its ID registers 0xFD..0xFF read `83 11 01` |
| Device, register state | Idle versus during a tone: register 0x12 goes from 0x02 to 0x00 and 0x0D from 0x02 to 0x01 (power-up of the DAC path), and the DAC volume register 0x17 reads 0xBF during playback. The dump is not compared with the datasheet |
| Device, by ear (user-reported) | A tone requested at 10 % of the cap (codec volume 5, about -47 dB on the library's curve) was silent or crackled; a 3 s tone at 100 % of the cap (codec volume 50) was heard as a steady tone; the melody was heard ("Słyszałem melodię") |
| No faults | Status after every run: `errors=0`; home application kept running; light sleep refused while a tone plays (by design) |

**Fact learned:** the library's volume scale is logarithmic. Volume 5 is nearly
inaudible, and the cap of 50 is about -25 dB. The cap is still an assumption:
the user did not report the level as too loud or too quiet.

**Not verified (PENDING).** Whether the melody is clean (no crackles between notes,
no clipping at the cap) and whether the level suffices. Behaviour of I2S and the
codec across light sleep. Pops at the start and end of a tone. The register
dump against the ES8311 datasheet.

## Gate A2 results, 2026-11-04 (firmware 0.3.47)

**Implemented.** `stream.c` (host-tested, no ESP-IDF dependency): a single-producer
single-consumer byte ring, the playback policy and PCM conversion to stereo with a
per-stream Q15 gain. `audio.c`: one stream at a time, owned by the audio task.

- **Ring:** 16,384 B in PSRAM, about 256 ms of 16 kHz stereo. Only the audio task
  frees it.
- **Policy:** WAITING until 100 ms are buffered, then PLAYING. An empty ring plays
  silence (UNDERRUN). After 500 ms of underrun the codec is closed and the
  amplifier turned off (PAUSED). Playback resumes after another 100 ms prebuffer.
  Close either drains the ring or stops at once.
- **Handles and ownership:** each open stream gets a new generation. Write and
  close check it. `roundie_audio_release_owner` stops an owner's stream for an
  application unload (gate A3 will call it).
- **Volume:** master 70 % of the cap by default (`roundie_audio_set_master`);
  stream gain on top. Rates 8 to 48 kHz; mono or stereo, 16-bit.
- **Busy:** a tone, the melody and a stream share one busy flag, so light sleep
  is refused while any of them plays.
- **RA1 additions:** `STREAM <hz> <ms> <vol> <stall_ms>` starts a producer task that
  writes a tone into a stream and goes quiet for `stall_ms` half-way; `RELEASE`
  stops it as an unload would.

**Defects found on the device and fixed**

| Version | Observation | Cause and fix |
|---|---|---|
| 0.3.45 | The stream never left WAITING with a full ring | The audio task chose an unbounded queue wait before the stream opened. Opening a stream now posts a wake request |
| 0.3.46 | The GUI loop stopped during a stream (0 iterations/s against 100 idle and 100 during the melody); unloading home waited for the stream's end and failed with `ui-teardown-pending`; after a sleep refused because of audio, home did not return | `CONFIG_FREERTOS_HZ` is 100, so `pdMS_TO_TICKS(2)` and `(5)` are 0 ticks. The producer polling a full ring (priority 2) and the release wait busy-looped and starved the GUI (main task, priority 1). Every short wait is now one tick (10 ms) |

A misleading `dropped` counter (it counted each retry of a full ring) was
removed.

**Unexplained reset (hypothesis).** During the 0.3.46 installation the board
restarted unplanned after reaching STABLE. Safe Mode (reason `core`) then rolled
back the pending 0.3.46. Whether 0.3.45 or 0.3.46 faulted is unknown, and no core
dump is configured. Hypothesis: the busy loop above starved the GUI until its
watchdog fired; the stream left open on 0.3.45 would have kept it running.
After `roundie_safe.py clear` the reinstall of 0.3.46, and 0.3.47 afterwards,
started normally.

**Verified (0.3.47, OTA from `ota_0`, running `ota_1`, VALID, self-test passed)**

| Evidence | Result |
|---|---|
| Host | Ring (wrap, partial writes, two-thread order), policy (prebuffer, underrun, pause, resume, drain), PCM conversion and gain. Full suite: 90 tests OK |
| Target build | No new warnings |
| Device, 3 s stream | Phase PLAYING with the ring near full; closed after about 3.3 s with the ring drained; 0 underruns |
| Device, 800 ms producer stall | 1 underrun and 1 pause, playback resumed and the stream drained and closed |
| Device, `RELEASE` mid-stream | Stream closed within 0.5 s |
| Device, GUI | 93 GUI iterations/s during a stream, 92 idle |
| Device, unload during a stream | Home exit reached the list within 1 s, no runtime error |
| Device, sleep during a stream | Refused (`app`), home relaunched (generation 2). Without audio, a 3 s sleep was entered |
| Device, by ear (user-reported) | Two 5 s streams with a 1.5 s producer stall, at codec volume 35 (master 70 % of the cap): not heard on the first play; after a replay, "Ok, słyszałem". Whether the gap itself was heard was not stated |
| No faults | `errors=0` after 10 plays; no Safe Mode; no reset |

**Not verified (PENDING).** Pops at pause and resume. Whether the default master
level of 70 % is loud enough (the first play was not heard). Streams at rates
other than 16 kHz and mono input on the device (host-tested only). Long streams
and repeated open and close (gate A4). Audio across light sleep.

## Gate A3 results, 2026-11-04 (firmware 0.3.48, `org.roundie.sound` 1.0.0)

**Decision: `rep_audio_v1`** (`include/rep/rep_audio_v1.h`), a second ABI 3 import
beside `rep_abi_v3`, like `rep_storage_v1`. The 52-byte `rep_api_v3_t` is
unchanged. Firmware without the import rejects the ELF with
`import:rep_audio_v1` before loading, so no capability bit is needed.

- **Table:** `open(instance, rate, channels, volume)` returns a handle or 0;
  `writable` returns the bytes that fit or -1; `write` copies whole frames and
  never blocks; `close(drain)` ends the stream. `buffer_bytes` reports the ring
  size (16,384).
- **NULL** when audio is not initialised (or `CONFIG_ROUNDIE_AUDIO` is off), and
  outside a callback. The app then runs silent.
- **Ownership:** one stream per instance; the system-wide busy flag also refuses a
  second stream or a stream during a system sound. Calls check the executor,
  the instance and the handle. `write` accepts only app data sections or SDK
  allocations, not the stack. The ring copies the samples, so no app pointer
  survives the call.
- **Release:** after the app's stop callback the runtime calls
  `roundie_audio_release_owner(instance, 200 ms)`; unload calls it again. The app
  may close its own stream in stop. A stream outlives neither stop nor unload.
- **Feeding:** apps use their periodic timer (50 ms minimum). The ring holds
  256 ms at 16 kHz stereo and 85 ms at 48 kHz stereo; above 16 kHz stereo a 50 ms
  timer leaves little margin.
- **Prebuffer cap (A2 fix):** 100 ms of 48 kHz stereo is 19,200 B, more than the
  ring, so playback would never have started. The prebuffer is now at most half
  the ring. Not host-tested (it lives in `audio.c`) and not run on the device.
- **Diagnostic owner:** the RA1 self-test stream now uses owner `UINT32_MAX`
  instead of 0xA2, which an application instance number could reach.

**Example:** `examples/sound_elf` ("Dźwięk"). A button plays "Wlazł kotek na
płotek" (a folk tune, 18 notes, about 6.4 s, 16 kHz mono) from a 256-entry
integer sine table with 5 ms fades, and stops it on a second press. Each 50 ms
tick writes what fits; at the end it closes with drain. No libc or libgcc
helpers (no 64-bit division). Without audio the button is disabled and the title
reads "Brak dźwięku".

**Verified**

| Evidence | Result |
|---|---|
| Host, `tests/test_app_round.c` (mocked audio backend) | Press fills the ring; ticks feed until the whole tune (203,520 B) is written, then close with drain; peak within 16,000; a second press stops without drain; a second open, another instance, a stale handle, a stack pointer and NULL are refused; NULL outside a callback; unload while playing releases the stream once; no audio gives a disabled button |
| Host, `tests/test_elf.c` | Resolver returns `rep_audio_v1` only for ABI 3; allowlist accepts it and refuses `rep_audio_v2` and `roundie_audio_stream_open` |
| Host | Full suite: 90 tests OK. Header compiles for Xtensa in C11 and C23 |
| Target | 0.3.48 builds without new warnings (1,132,272 B). Example: 5,496 B, imports `rep_abi_v3` and `rep_audio_v1` only, accepted by the validator |
| Device, install | OTA VALID, running `ota_0`; package SHA-256 `490abfcd…3781` installed; 3 widgets, 0 UI failures |
| Device, injected press | Tune played to the end: ring kept at 14–16 KB, 0 underruns, 0 pauses, stream closed itself; GUI 100 iterations/s; longest app callback 4.3 ms, no budget overrun |
| Device, exit mid-tune | Stream released within 1.5 s (`plays` +1, no stream open), launcher back to home, no runtime error |
| Device, by ear (user-reported) | Pressed with a finger (UI counters: 4 physical presses, 0 injected, 0 failures): "Słychać melodię" |

**Not verified (PENDING).** Rates other than 16 kHz and stereo from an app on
the device. Pops at start and end. Light sleep while an app holds a stream (it is
refused while audio is busy, as for any sound; not exercised with the app).
Repeated launch and unload while playing (gate A4).
