# Installing Sample Studio

Sample Studio is part of the firmware you build: an SDK build with the
`sample_studio` extension in it. Nothing here is firmware. You build your own,
from the OS on your own OP-XY, on Windows, macOS or Linux.

These steps follow the SDK's own [build guide](https://github.com/etcetc000/open-xy/blob/staging/docs/build.md),
which has more detail on each.

## What you need

- An OP-XY running OS 1.2.1, and a USB cable.
- Python 3.11 or newer, and git.
- The Open-XY SDK with the `sample_studio` capability. Until it is part of the
  SDK's `staging` branch, it is on the `sdk/sample-studio-capability` branch of
  [mmockett/open-xy](https://github.com/mmockett/open-xy).
- This repository, beside the SDK.

```sh
git clone -b sdk/sample-studio-capability https://github.com/mmockett/open-xy
git clone https://github.com/mmockett/OP-XY-SampleStudio
cd open-xy
python sdk.py toolchain fetch
```

`toolchain fetch` downloads the pinned Blackfin compiler for Windows x86_64
and macOS arm64. On Linux, build it as the SDK's
[build guide](https://github.com/etcetc000/open-xy/blob/staging/docs/build.md)
describes.

## 1. Copy the OS from your OP-XY

1. Hold COM while switching the OP-XY on. It shows the boot menu, which must
   list "3. dev menu"; if it doesn't, the SDK's builder has a helper to turn
   it on.
2. Press track 3 for "3. dev menu", then track 3 for "3. mtp file transfer
   (windows)", or track 2 for "2. mtp file transfer (mac)". Wait for "mtp mode
   active".
3. Open the OP-XY in File Explorer, or in OpenMTP on a Mac (quit Field Kit
   first). On Linux, use track 3 and your file manager.
4. Copy `/upgrade/firmware/blackfin/bin/os.ldr` to a folder of your own,
   outside both checkouts, for example `stock-1.2.1/os.ldr`. Keep it: it is also
   how you go back to stock.
5. Press track 2 for "2. exit".

## 2. Build

A build needs at least one machine besides the extension. Fetch every
published machine:

```sh
python sdk.py catalog fetch
```

This writes `local/catalog/collection.json`. Copy it to `my-collection.json`
in the same folder and keep only the machines you use; all of them, with
Sample Studio, share the firmware's 2032 KiB, and `python sdk.py compile`
shows each one's share.

Keep at least one sample engine (such as `timestretch` or `grains`), or add the
Drumline page 2 extension: the studio reads samples through the service they
bring, and the build says so if none is there.

If your OP-XY already runs a firmware built with the SDK or open-xy.com, copy
its `/layout.json` from the device too, and pass it with `--layout`: your
saved projects then keep their machines.

```sh
python sdk.py build \
  --collection local/catalog/my-collection.json \
  --extension ../OP-XY-SampleStudio/sample_studio \
  --stock-os <your folder>/stock-1.2.1/os.ldr \
  --profiling off \
  --out <builds>/studio-001
```

Use a new `--out` folder for every build, outside the checkouts. The build
checks the result and lists the files to copy, under `<out>/candidate/`.

## 3. Copy the build to the OP-XY

Open file transfer again (step 1, items 1 to 3), then:

```sh
python sdk.py device push <builds>/studio-001/candidate
```

`device push` backs up each file it replaces, reads every copied file back,
and puts the backups back if anything fails. On Windows it needs
`python -m pip install comtypes==1.4.17`; on macOS and Linux, the libusb
library (`brew install libusb`, or `apt install libusb-1.0-0`), the "mtp file
transfer (mac)" mode, and no other app holding the device.

You can also copy the files by hand: everything under `candidate/` goes to the
same path on the device, replacing what is there.

## 4. Write the OS

1. Press track 2 for "2. exit".
2. Press track 5 for "5. write os to boot partition", keeping the OP-XY on
   power.
3. When "firmware written ok!" appears, switch it off and on.

A plain restart keeps the old OS. If you have already left file transfer, hold
COM while switching on, then press track 3 and track 5.

Sample Studio is then in the sample library: Shift + Sample, highlight a
sample, and press `edit`.

## Going back to stock

1. Open file transfer (step 1, items 1 to 3).
2. Copy your saved `stock-1.2.1/os.ldr` to `/os.ldr`, and the SDK's
   `profiles/opxy-1.2.1/content/op1_factory.json` to
   `/content/op1_factory.json`, replacing both. `device push` keeps backups of
   the files it replaced in step 3; these are the same two files.
3. Write the OS (step 4).

Files the studio saved stay on the OP-XY: edits beside their originals, slices,
and kits in `presets/snapshot`. Its scratch folder, `samples/user/.studio`, can
be deleted.
