# Editor-closed waveform and callback reproduction

The Windows SDK host `skellytune_vst3_stress` supports these optional environment
variables (presence enables them):

- `SKELLYTUNE_STRESS_PACED`: process on a paced worker with the message pump on
  the main thread, even when the editor is closed.
- `SKELLYTUNE_PREVIEW_UI`: open the editor and use the same paced worker.
- `SKELLYTUNE_STRESS_TRACE`: absolute CSV filename for per-callback timings.
- `SKELLYTUNE_STRESS_RENDER`: capture interleaved stereo float32 output to
  `<prefix>-output.raw`. Storage is allocated before processing and written
  after processing. The file uses the command-line sample rate.

Unset `SKELLYTUNE_PREVIEW_UI` for closed-editor reproduction. Keep identical
inputs, parameters, rate and block size for paired runs. Use `--measure-startup`
to include the first half-second; the default excludes it from timing statistics.
The profile file records whether pacing, the editor and output capture were enabled.

Callback wall duration and processing-thread cycles are measured around the
component process call. Output copies occur outside that measured interval.
Pacing uses the OS sleep scheduler, not an audio device. These results do not
establish DAW dropouts, hardware round-trip latency or wake-up deadline accuracy.
Late calls do not insert missing samples into the captured output: compare repeat
renders to distinguish deterministic waveform artifacts from timing variability.

Keep private vocal inputs and all generated output outside the public checkout.
No engine tracing or file writing is added to the plugin's audio callback.
