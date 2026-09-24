# Tests

[docs/testing.md](../docs/testing.md) describes the tests: `make test`, the
unit test support headers, the boot tests, the fixtures, the integration
scripts and the scratch and FUSE rules.

In short:

- `make test` runs the unit tests, the Bundles page and the bootstrap tests.
  It needs no game, no disc and no runtime, and it is what CI runs.
- The scripts in `tests/integration/` are run by hand against real builds,
  runtimes and your own discs. They skip cleanly when what they need is
  missing.

## Local configuration

The games the integration tests use are yours, so which ones is never
committed. The scripts read them from `tests/local.env`, which is gitignored:

```sh
cp tests/local.env.example tests/local.env
$EDITOR tests/local.env
```

`tests/local.env.example` explains each variable, and
[docs/testing.md](../docs/testing.md#local-configuration) lists them.
`tests/local/` is gitignored too, for expectation files such as the listing
`scan.sh` checks against (`tests/local/expected-collection.txt` by default):
one line per image or archive, with its file name and how many discs are in
it, separated by a tab.

```
Example.zip	2
Another Game (USA).iso	1
```

Manifests (`games/`) and the known-disc list (`db/discs.txt`) are local data
in the same way.

Run `make check-generic` before sending a change. It fails if any tracked file
names something from your local collection.
