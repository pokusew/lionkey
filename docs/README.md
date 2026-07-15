# Website and Documentation

👉 Available online at [lionkey.dev]

The website and documentation is built using [Docusaurus], a modern static website generator.

The content is written in Markdown.

## Local Development

### Requirements

- [Node.js] >=24
- [npm] (comes with Node.js)
- You can follow our [Node.js Development Setup guide].

### Set up

First, install the dependencies using [npm]:

```bash
npm install
```

### Run

```bash
npm run start
```

This command starts a local development server and opens up a browser window.
Most changes are reflected live without having to restart the server.

### Build

```bash
npm run build
```

This command generates static content into the `build` directory
and can be served using any static contents hosting service.

## Deployment

To [Cloudflare Workers] using [Wrangler]:

```bash
# first build
npm run build
# then deploy
npm run deploy
```

<!-- links references -->

[lionkey.dev]: https://lionkey.dev/
[Node.js]: https://nodejs.org/en/
[npm]: https://www.npmjs.com/
[Node.js Development Setup guide]: https://lionkey.dev/docs/development/nodejs
[Docusaurus]: https://docusaurus.io/
[Cloudflare Workers]: https://workers.cloudflare.com/
[Wrangler]: https://developers.cloudflare.com/workers/wrangler/
