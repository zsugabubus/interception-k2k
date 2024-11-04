# interception-k2k

All-in-one input key mapper for [Interception Tools](https://gitlab.com/interception/linux/tools).

## Configuration

User configuration files can be placed under subdirectories of `examples` directory, or in a new dir next to it. Executables will be generated per subdirectory, so you can easily manage your rules if you need more instances of `interception-k2k`.

This repository contains the following example configurations:

### caps2esc

This maps <kbd>caps lock</kbd> to <kbd>esc</kbd> when tapped and to <kbd>left control</kbd> when held.

### interception-pipe0

This contains three distinct setups:

- Caps2esc, like before.
- Home-row modifiers, mapping <kbd>a</kbd>,<kbd>s</kbd>,<kbd>d</kbd>,<kbd>f</kbd>,<kbd>j</kbd>,<kbd>k</kbd>,<kbd>l</kbd>,<kbd>;</kbd> and <kbd>space</kbd> to <kbd>control</kbd>, <kbd>alt</kbd>, <kbd>meta</kbd> and <kbd>shift</kbd> when held.
- Vim overlay, ...

### interception-pipe1

This contains these distinct setups:

- <kbd>control</kbd> locks when both keys are pressed together. The same for <kbd>meta</kbd>.
- Shift2caps, like below.
- <kbd>left meta</kbd> key combinations as media keys

### qwerty-ws

This remaps the right half of an ANSI laptop keyboard after you have moved around some of its keycaps for a WideSym mod (inspired by [DreymaR's Colemak-CAWS](https://dreymar.colemak.org/ergo-mods.html#symbols)).

### shift2caps

This toggles <kbd>caps lock</kbd> when both <kbd>shift</kbd> keys are pressed together.

### udevmon.yaml

If you wish to try out one or more of these example configurations, copy `udevmon.yaml` to `/etc/interception/`. Multiple configurations can be chained that yaml:

```yaml
- JOB: "intercept -g $DEVNODE | /opt/interception/interception-pipe0 | /opt/interception/interception-pipe1 | uinput -d $DEVNODE"
  DEVICE:
    EVENTS:
      EV_KEY: [KEY_CAPSLOCK, KEY_ESC, KEY_SPACE]
```

## Installation

```sh
git clone https://github.com/zsugabubus/interception-k2k &&
cd interception-k2k &&
make && # or make CONFIG_DIR=<your new dir>
make install
```

By default `make` builds all configurations in the `examples` directory. Add `CONFIG_DIR=<your new dir>` if you created a new dir that only contains your configurations to prevent `make install` from installing any examples you might not plan on using.
