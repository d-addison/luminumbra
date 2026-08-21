# Banso audio extension

This extension exposes the `audio-bank-validate` step. The step delegates all
validation to the repository validator, preserves its JSON findings as the step
output, and fails whenever the validator exits nonzero.

## Run the validator standalone

Run this command from the repository root:

```sh
python tools/audio/bank_validate.py --json
```

The command writes a JSON report containing `valid`, `summary`, and `findings`.
An exit status of zero means validation passed. Any finding produces a nonzero
exit status.

## Use it as a pipeline preflight

Reference this extension directory from the pipeline and add its step to the
preflight list:

```yaml
extensions:
  - path: tools/banso-audio

preflight:
  steps:
    - uses: banso-audio/audio-bank-validate
```

Run the pipeline from the repository root so the step can invoke
`tools/audio/bank_validate.py` and resolve bank and asset paths consistently.

## Test the extension

```sh
npm --prefix tools/banso-audio test
```
