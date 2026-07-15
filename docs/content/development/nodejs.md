---
sidebar_position: 3
description: A simple guide to setting up Node.js using nvm
sidebar_label: Node.js
---

# Node.js Development Setup

We'll install a recent version of [Node.js] using [nvm] (Node Version Manager),
which is a Node.js version manager for Unix-like/POSIX systems such as Linux and macOS.

:::info

For other options and systems (e.g., **Windows**), please refer to the official [Node.js Download page].

:::

1. Install [nvm] using Git:

   ```bash
   cd ~/
   git clone https://github.com/nvm-sh/nvm.git .nvm
   cd ~/.nvm
   git checkout v0.40.5
   ```

2. Add the following at the end of your `~/.bashrc`:

   ```bash
   ###
   # nvm
   # source: https://github.com/nvm-sh/nvm#git-install
   ###
   export NVM_DIR="$HOME/.nvm"
   [ -s "$NVM_DIR/nvm.sh" ] && \. "$NVM_DIR/nvm.sh"                   # This loads nvm
   [ -s "$NVM_DIR/bash_completion" ] && \. "$NVM_DIR/bash_completion" # This loads nvm bash_completion
   ```

3. Restart your terminal. Verify nvm works (should print something like `0.40.5`):

   ```bash
   nvm -v
   ```

4. Install the latest Node.js 24:

   ```bash
   nvm install 24.*
   ```

5. Verify Node.js is installed and active (should print something like `v24.18.0`):

   ```bash
   node -v
   ```

6. Upgrade the bundled npm:

   ```bash
   npm --global install npm
   ```

7. Check the installed npm version:
   ```bash
   npm -v
   ```

That's all!

<!-- links references -->

[nvm]: https://github.com/nvm-sh/nvm
[Node.js]: https://nodejs.org/en/
[npm]: https://www.npmjs.com/
[Node.js Download page]: https://nodejs.org/en/download
