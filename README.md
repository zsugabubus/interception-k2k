# interception-k2k

All-in-one input key mapper for [Interception Tools](https://gitlab.com/interception/linux/tools).

## Configuration

User configuration files can be placed under subdirectories of `examples` directory, or in a new dir next to it. Executables will be generated per subdirectory, so you can easily manage your rules if you need more instances of `interception-k2k`.

- For simple 1-to-1 mappings, use `map-rules.h.in`.
  - If you want to disable a key, map it to `KEY_RESERVED`.
- For many-to-1, use `multi-rules.h.in`.
- Note that there does not seem to be a way to map a single key input to output multiple keys. Use dual-function-keys for that.
- For different behavior when a key is tapped and when it's held, use `tap-rules.h.in`.

This repository contains the following example configurations:

### caps2esc

This maps <kbd>caps lock</kbd> to <kbd>esc</kbd> when tapped and to <kbd>left control</kbd> when held.

### ctrl-meta-lock

<kbd>control</kbd> locks when both keys are pressed together. The same for <kbd>meta</kbd>.

### disable-keys

This lets you disable certain keys that you have mapped elsewhere, forcing you to adjust to their new location. If you are chaining configurations, make sure you disable first, then add the new mapping.

### home-row-mods

Mapping <kbd>a</kbd>,<kbd>s</kbd>,<kbd>d</kbd>,<kbd>f</kbd>,<kbd>j</kbd>,<kbd>k</kbd>,<kbd>l</kbd>,<kbd>;</kbd> and <kbd>space</kbd> to <kbd>control</kbd>, <kbd>alt</kbd>, <kbd>meta</kbd> and <kbd>shift</kbd> when held.

### media-keys

<kbd>left meta</kbd> key combinations as media keys.

### qwerty-ws

This remaps the right half of an ANSI laptop keyboard after you have moved around some of its keycaps for a WideSym mod (inspired by [DreymaR's Colemak-CAWS](https://dreymar.colemak.org/ergo-mods.html#symbols)).

### shift2caps

This toggles <kbd>caps lock</kbd> when both <kbd>shift</kbd> keys are pressed together.

### vim-overlay

Holding <kbd>e</kbd> activates vim-like functions on the right side of the keyboard, and holding <kbd>i</kbd> activates some on the left. Note that this mapping seems to be incorrect, so it would need some work before these are actual vim keys. The correct ones look to be commented out.

### udevmon.yaml

If you wish to try out one or more of these example configurations, copy `udevmon.yaml` to `/etc/interception/`. Multiple configurations can be chained in that yaml:

```yaml
- JOB: "intercept -g $DEVNODE | /opt/interception/caps2esc | /opt/interception/shift2caps | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_CAPSLOCK, KEY_ESC, KEY_SPACE]
```

Note that performance-wise it may be a good idea to combine your configurations in a single executable (i.e. subfolder) instead of chaining multiple configurations.

## Installation

```sh
git clone https://github.com/zsugabubus/interception-k2k &&
cd interception-k2k &&
make &&
make install
```

By default `make` builds all configurations in the `examples` directory. Add `CONFIG_DIR=<your new dir>` if you created a new dir that only contains your configurations to prevent `make install` from installing any examples you might not plan on using.

By default `make install` copies the executables to `/opt/interception`. Add `INSTALL_DIR=<somehwere else>` if you want to change that.

All together this may look like:

```sh
make clean
make CONFIG_DIR=in
sudo make install CONFIG_DIR=in INSTALL_DIR=/usr/bin
```
