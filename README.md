# interception-k2k
All-in-one input key mapper for [Interception Tools](https://gitlab.com/interception/linux/tools).

## Configuration

User configuration files can be placed under subdirectories of `default`
directory. Executables will be generated per subdirectory, so you can easily
manage your rules if you need more instances of `interception-k2k`.

If you wish to try out example configuration, modify `/etc/udevmon.yaml` to
look something like this:
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
make && # or make CONFIG_DIR=caps2esc
make install
```
