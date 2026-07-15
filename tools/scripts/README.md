# Tools Scripts

This directory contains scripts and utils written in [TypeScript].

## Content

<!-- **Table of Contents**  *generated with [DocToc](https://github.com/thlorenz/doctoc)* -->
<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->

- [Setup](#setup)
- [FIDO Conformance Tools Automation Server](#fido-conformance-tools-automation-server)
- [CTAP 2.1 PIN/UV Auth Protocol](#ctap-21-pinuv-auth-protocol)
- [Intel HEX Files Utils](#intel-hex-files-utils)
- [Other Commands](#other-commands)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

## Setup

**Requirements:**

- [Node.js] >=20
- [npm] (comes with Node.js)
- You can follow our [Node.js Development Setup guide].

Install the dependencies using [npm]:

```bash
npm install
```

## FIDO Conformance Tools Automation Server

This is simple Node.js server that implements the FIDO Conformance Tools [Automation API]. This server
connects to the authenticator via the **serial port** and utilizes LionKey's UART Debugging Interface to send the
appropriate
commands to the authenticator whenever it receives an automation HTTP POST request. Additionally, it forwards all
standard input and output to and from the serial port, so it **can be used as a serial console**.

Usage:

```bash
node --import tsx src/fido-conformance-tools-automation-server.ts {serialDevice} {baudRate}
```

For LionKey, use `115200` as the baudRate.

For example:

```bash
node --import tsx src/fido-conformance-tools-automation-server.ts /dev/tty.usbmodem141202 115200
```

## CTAP 2.1 PIN/UV Auth Protocol

We implemented both PIN/UV Auth Protocol versions in TypeScript/Node.js so that we could use them as a reference
(and to generate test data for unit tests) when implementing them in C within LionKey.

The PIN/UV Auth Protocol TypeScript/Node.js implementation is
in [src/pin-uv-auth-protocol.ts](./src/pin-uv-auth-protocol.ts).
A simple test data generator CLI (with hardcoded authenticator public key and platform private key)
is in [src/test.ts](./src/test.ts).

Generate test data for the authenticatorClientPIN **[setPIN (0x03)]** subcommand:

```bash
node --import tsx src/test.ts {v1|v2} setPin {newPin}
```

Generate test data for the authenticatorClientPIN **[changePIN (0x04)]** subcommand:

```bash
node --import tsx src/test.ts {v1|v2} changePin {oldPin} {newPin}
```

## Intel HEX Files Utils

See [src/hex-file.ts](./src/hex-file.ts).

## Other Commands

See also the [scripts section in package.json](./package.json#L6).

- `npm run check-format` – Checks if all the code is correctly formatted with [Prettier] (`prettier . --check`).
- `npm run format` – Formats the code using [Prettier] (`prettier . --write`).
- `npm run lint` – Runs [ESLint]. Outputs errors to console. See [the ESLint config](./eslint.config.mjs).
- `npm run tsc` – Runs TypeScript compiler (`tsc`) only for typechecking and outputs type errors to console.

<!-- links references -->

[Node.js]: https://nodejs.org/en/
[npm]: https://www.npmjs.com/
[Node.js Development Setup guide]: https://lionkey.dev/docs/development/nodejs
[TypeScript]: https://www.typescriptlang.org/
[ESLint]: https://eslint.org/
[Prettier]: https://prettier.io/
[Automation API]: https://github.com/fido-alliance/conformance-test-tools-resources/blob/main/docs/FIDO2/Automation.md
[setPIN (0x03)]: https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#settingNewPin
[changePIN (0x04)]: https://fidoalliance.org/specs/fido-v2.1-ps-20210615/fido-client-to-authenticator-protocol-v2.1-ps-errata-20220621.html#changingExistingPin
