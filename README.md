# mpc-addin-sample-draw-handler

An addin for the MPC Sample that owns the screen on behalf of plugins that draw on it. MPC's own UI on the Sample has no
plugin page, so a plugin built for it can draw a full-screen page of its own: the addin decides which plugin may draw and
puts its pixels on the panel.

- **One owner at a time, the last to ask.** A plugin opens a client, draws into its own 320x240 layer and asks for the
  screen. It gets the whole screen at once (a previous owner is told it lost it) and keeps it until it releases it, when its
  page closes. Then MPC's screen comes back as MPC last drew it. Nothing is handed over by itself.
- **No link dependency.** The interface is `src/mpc_sample_draw.h`; a plugin looks up `mpc_sample_draw_api` with `dlsym`, so
  it still loads where the addin isn't installed. The drawing side for plugins lives in mpc-vst-plugins' wrapper.
- **How:** the addin wraps `ioctl` in MPC. MPC double-buffers and page-flips when its UI changes; on each flip the addin
  saves MPC's new frame and paints the owner's layer over it. A client's `present()` copies its rectangle onto the screen
  and refreshes it on MPC's display fd (only the DRM master may). See mpc-vst-plugins docs/NOTES.md, 2026-11-04.
- **Control** (on the device, `127.0.0.1:6740` by default; from a computer `ssh -L 6740:127.0.0.1:6740 root@<device>`):
  `/status`, `/grant?id=N`, `/release`, `/test?on=1|0` (a built-in test pattern).

Only starts in the MPC Sample's process (`/usr/bin/MPC Sample`). Settings: `mpc_sample_draw_handler.conf` next to the
library (`enabled`, `bind`, `port`).

Build and test: `tests/test.sh` (host, ASan/UBSan, a file as the framebuffer), `./build-aarch64.sh`, then
`tools/release-aarch64.sh <version>` (needs mpc-vst-plugins beside this repo). Install with the zip's `install.sh`.

Known gaps (0.1.0): MPC's pixels that a client's `present()` overwrites before MPC's next flip are not in the saved frame,
so after release they show the owner's pixels until MPC redraws them. Like every addin on the Sample, it is loaded through
a drop-in in `/run` and is gone after a reboot until that is solved.
