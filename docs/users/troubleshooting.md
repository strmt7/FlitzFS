# Troubleshooting

[Documentation](../README.md) · [Getting started](getting-started.md)

## There is no installer or Explorer drive

There is no public release or supported installation yet. The experimental
application works with its own temporary test volume. Full Explorer integration
and physical-volume setup are still in development.

## The GPU is available, but ordinary operations use CPU

Automatic GPU routing is unfinished. Experimental GPU compression results do
not mean the current application sends ordinary file operations to the GPU.
Changing drivers is not a remedy for an unimplemented application feature.

## Optimize does not run automatically

Optimize is currently an explicit experimental operation. Automatic grouping,
learning and background maintenance are planned features.

## Closing the window did not exit

The application can hide to its tray icon while retaining its temporary volume.
Use the application's discard and exit controls. Forced termination loses
temporary volume contents.

## A save, allocation or compression operation is refused

Keep the error text and the exact action that led to it. Do not treat a refusal
as successful storage or delete an original file. Experimental capacity and
resource limits may prevent an operation.

## Report a problem

Use [GitHub Issues](https://github.com/strmt7/FlitzFS/issues) with:

- The build identifier and Windows version.
- The action, expected result and observed result.
- The complete error message and a small reproducible example.

Remove personal data and credentials from reports. Developers should follow
the [developer guide](../development/README.md) for controlled reproduction;
do not improvise driver or volume changes to investigate a failure.
