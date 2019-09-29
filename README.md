# interception-k2k
All-in-one input key mapper for [Interception Tools](https://gitlab.com/interception/linux/tools).

## Configuration

User configuration files can be placed under subdirectories of `config`
directory. Executables will be generated per subdirectory, so you can easily
manage your rules if you need more instances of `interception-k2k`. See
`example` directory for examples.

## Installation

```sh
git clone https://github.com/zsugabubus/interception-k2k
cd interception-k2k
make # or make CONFIG_DIR=example
make install
``
