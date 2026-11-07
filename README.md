# N3V Player

A native video player for the Nintendo 3DS (old and New). Convert any video on your PC into an `.n3v` file, copy it to the SD card, and play it from a simple file browser.

There is no video codec on the 3DS side. The PC does the heavy work up front, and the 3DS only runs cheap native C:

- **Picture:** 400x240, the top screen's own size, as 4x4 two-colour blocks. Only blocks that changed are stored, packed with zlib. The CPU draws them straight into the framebuffer.
- **Sound:** stereo IMA ADPCM at 32728 Hz, decoded by the CPU and streamed to the DSP. The sound's sample clock picks the frame, so picture and sound stay in sync.
- **No pauses:** a reader thread keeps several seconds of video and sound read ahead from the SD card, so a slow read never stalls playback.
- **Seeking:** a keyframe every ~2 seconds.

## Install

Download the latest **`N3VPlayer-x.y.z.zip`** from [Releases](../../releases/latest) and unzip it. Inside:

- `N3VPlayer.cia`: install it with FBI. Or copy `N3VPlayer.3dsx` to `sdmc:/3ds/` and open it from the Homebrew Launcher. You only need one of the two.
- `converter/`: the video converter (see below).

Sound needs `sdmc:/3ds/dspfirm.cdc`, which most homebrew setups already have (dump it with DSP1 if not).

## Convert a video

**Windows, easiest:** double-click `converter/setup.bat`. The first time, it installs everything the converter needs: Python 3.12 from python.org (only if you don't have Python 3.9+ already), then numpy and imageio-ffmpeg (which brings its own ffmpeg) into a private `.venv` folder beside it. Then it opens the converter window: add videos, see exactly where each `.n3v` will be saved (next to the video, or a folder you pick), choose the picture quality and press **Convert**. Later runs open the window straight away. You can also drag videos onto `setup.bat` or `convert.bat` to start with them in the list.

**Any system, by hand:** needs Python 3.8+ and two packages:

```
pip install numpy imageio-ffmpeg
python converter/n3vconvert.py "my video.mp4"
```

Run it without a file name (or with `--gui video1.mp4 ...`) for the converter window. Anything ffmpeg can read works (MP4, MKV, WebM, AVI...). Videos above 30 fps are halved (60 becomes 30), and the picture is fitted to 400x240 with black bars.

Options: `--quality 1`..`5` (default 3; higher means a better picture and bigger files), `--preview N` saves frame N as the 3DS will show it.

Copy the `.n3v` files anywhere on the SD card. The player starts in `sdmc:/videos/` if that folder exists.

## Controls

| Browser | |
|---|---|
| D-pad up/down | move |
| D-pad left/right | page |
| A | open folder / play |
| B | up a folder |
| START | quit |

| Playing | |
|---|---|
| A | pause |
| L / R | back / ahead 10 s |
| D-pad up/down | ahead / back 1 min |
| B | stop, back to the list |
| START | quit |

## Build

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) (devkitARM, libctru, 3ds-zlib), plus `makerom` and `bannertool` for the CIA.

```
make                                         # N3VPlayer.3dsx
CTRTOOLS=/path/to/tools ./build.sh           # also dist/N3VPlayer.cia
```

`./build.sh autotest` builds a version that plays the first video in the list by itself (handy in an emulator).

## The .n3v format

Described at the top of [`converter/n3vconvert.py`](converter/n3vconvert.py): a 64-byte header, the whole ADPCM soundtrack, the frames (each a length plus zlib data), then a table of keyframe offsets.

## License

MIT. Only convert videos you have the right to copy.
