# Contributing to `corosched`

Thanks for your interest in contributing!
This document covers how to submit changes and report bugs, the AI policy and licensing.

## Submitting changes

- Adhere to [conventional commits](https://www.conventionalcommits.org).
- Add or update tests for any change in behavior, and run the test suite (see [README](README.md)).
- Keep pull requests small and focused on one change.

## Reporting bugs

Include the version, steps to reproduce, and expected vs. actual behavior.
The best bug report is a failing test.

For security issues, contact [jacopo@delgranchio.it](mailto:jacopo@delgranchio.it) privately instead of opening a public issue.

## AI policy

AI tools are welcome for some parts of the project and not others.
In every case, **you are the contributor**, and the tool is not.

| Area                                                 | AI use  | Disclosure                      |
|------------------------------------------------------|---------|---------------------------------|
| Reading, explaining, reviewing, bug hunting          | Yes     | Not required                    |
| Source code                                          | Depends | See [Source code](#source-code) |
| Tests                                                | Yes     | `Assisted-by:` commit trailer   |
| Examples                                             | Yes     | `Assisted-by:` commit trailer   |
| Docs: light edits (grammar, translation, docstrings) | Yes     | Not required                    |
| Docs: substantially generated                        | Yes     | `Assisted-by:` commit trailer   |

### Always disclose

If an AI tool wrote any part of your contribution, add a trailer to the commit message:
```
Assisted-by: <tool> (<model>)
```
Don't use `Co-authored-by:` for AI tools. You are the author.

> **Rule of thumb:** disclose anything a tool wrote for you.
> Disclosed or not, only submit what you can explain and defend line by line.
> Tools that only polish your own work (spellcheck, grammar fixes, completing the line you're typing) don't need disclosure.

### Source code

How much AI-written source gets accepted depends on the change:

- Small, _focused_ changes (a bug fix, a small feature, a local refactor) are likely fine.
- Larger changes (new features, cross-cutting refactors, anything touching the core design) will have a harder time being accepted.

These limits exist because the maintainers carry the review and maintenance cost, not as a judgment on the tools.

Undisclosed AI use that comes to light may lead to the change being reverted.

### Agents

No unattended submissions: a human must review and submit every pull request, issue and comment.
Accounts that post AI output without a human in the loop may be blocked.

Instructions for AI agents working in this repository are in [AGENTS.md](AGENTS.md).
Keep your personal configuration out of it, as it's shared by everyone.

## Licensing

- The project is licensed under the [Mozilla Public License 2.0](LICENSE) (MPL-2.0).
- **Exception:** examples and docs code samples are licensed under [MIT-0](examples/LICENSE), so they can be used with no obligations.

Each file starts with an SPDX header using the comment syntax of the file's language.

Library and test files:
```
// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */
```

While for examples:
```
// SPDX-License-Identifier: MIT-0
```

By submitting a contribution, you agree that it is licensed under the license of the files it touches.
You also make the representation in MPL-2.0 §2.5: you believe the contribution is your original creation, or you have sufficient rights to license it.
