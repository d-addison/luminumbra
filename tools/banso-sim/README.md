# Banso simulation extension

This extension exposes Luminumbra's typed simulation steps and a `simulation`
verification gate. The gate runs the `smoke` step and passes only when the server
process exits successfully, returns a non-empty `world_hash`, and leaves readable
stdout and stderr evidence. A server that is missing, cannot be started, exits
nonzero, omits its hash, or omits its raw output always produces a red result.

## Run the simulation domain standalone

Build the headless server first, then pass its path to the gate:

```powershell
npm --prefix tools/banso-sim run gate -- --server 'build/bin/luminumbra_server.exe'
```

Set `LUMINUMBRA_SERVER` instead of `--server` when the binary path is supplied by
the environment. Arguments after a second `--` are appended to the smoke run:

```powershell
npm --prefix tools/banso-sim run gate -- --server 'build/bin/luminumbra_server.exe' -- --seed 42 --ticks 600
```

The command prints a `banso.gate.result.v1` JSON report and returns nonzero for a
red gate. `reproduction_command` contains the exact server invocation used by an
operator to reproduce the run.

Use `--evidence-dir <directory>` to choose the retained evidence root. The default
is `.banso/evidence/simulation` relative to the working directory.

## Run inside Banso verification

The extension manifest declares the `simulation` custom gate between the
`implement` and `verify` phases. From the repository root, run the normal Banso
verification command:

```powershell
banso verify
```

Banso loads the extension, invokes the gate's `runSimulationGate` export, and
records its pass or fail report. The server binary can be supplied through the
`LUMINUMBRA_SERVER` environment variable used by the gate.

## Failure evidence

Every failure creates a unique retained directory and links it from `report`.
The directory contains:

- `stdout.log` and `stderr.log`: verbatim server output, or an explicit marker
  identifying output that was unavailable;
- `run-parameters.json`: working directory, binary, exact arguments, normalized
  step parameters, and reproduction command;
- `world-hash.txt`: the verbatim hash, or `null` when the result omitted it;
- `result.json`: the complete normalized smoke result;
- `manifest.json`: verdict, reasons, source evidence paths, and bundle members.

Failure bundles are never deleted by the gate. CI or operators can retain or
publish the linked directory using their normal artifact policy.

## Test the extension

```powershell
npm --prefix tools/banso-sim test
```
