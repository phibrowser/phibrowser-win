![Phi Browser](docs/assets/phibrowser_github_2000.png)

# Phi Browser for Windows

**The AI browser for Windows.** Agentic, local-first, native.

Phi is a Chromium-based browser built as a real native Windows app, with an AI agent that acts in the page, a memory that lives in a file you can read, and the option to run AI entirely on your own machine. It is built for something new: a browser a person and an AI agent can use *together*, in the same window and the same session, each a first-class user.

> **Early stage.** The Windows client is young. This is an early-access, early-bird release: the core browser is solid, but features are still landing and some will have rough edges. The macOS version is further along, and the two are converging fast. Bug reports and ideas count for a lot right now, so if something feels off, please tell us.

**Download Phi for Windows** — early-access builds are on the [Releases](../../releases) page. For macOS and the full story, see **[phibrowser.com](https://phibrowser.com)**.
If this is your kind of thing, a ⭐ helps a lot.

<br>

## What makes it different

- **Built for people who live in a browser.** Phi is an everyday browser made for heavy, all-day use, with the productivity power-ups that implies. It is also built so an AI agent can work beside you: seeing what you are working on, carrying the thread forward, and handing it back, in the same window and the same session rather than boxed off in a sandbox.
- **Simple until you want more.** Phi opens as a clean tabbed browser that feels at home on Windows, and nothing is in your way until you reach for it. Then it unfolds: spaces, profiles, split views, tab groups, URL rules, keyboard shortcuts, picture-in-picture, incognito spaces, and more.
- **The AI goes as deep as you want it to.** An agent that works in the page rather than in a chat box. Persistent memory that builds up as you browse. Skills the agent assembles from workflows you show it once. And the choice to point Phi at models running on your own machine, use Phi Cloud, or switch the AI off entirely and use Phi as a plain browser.
- **An agent with the same reach you have.** The agent sees what you see and picks up from where you are, so you rarely have to spell out every step. Most things inside a browser window today are full applications rather than pages, and the agent operates them the way you would, with the same capabilities as a human operator and a visible record of every action it took. Not a chat panel bolted onto a browser.
- **Memory you can actually read.** Phi's memory of your browsing is created and stored on your device and stays your property. You can open it, edit it, delete entries, or wipe it. We do not collect it and we do not upload it. We do not train AI models on your content.
- **Your AI, your choice of where it runs.** Point Phi at models on your own hardware through LM Studio or Ollama, with no account involved. Or use Phi Cloud. Or use none.
- **Independent, and nothing to lock you into.** Phinomenon is a small independent software house. We do not run a search business, an ad network, or a cloud we need to keep you inside, so our incentive is simply that you like the browser enough to keep it. Bring your own models, use ours, or use none.
- **A real browser underneath.** A genuine Chromium engine with a native Windows client on top: the window, tabs, sidebar and spaces are native Windows UI, not web views. A full browser that holds up as one, fast and at home on the desktop.

<br>

## What this repository is

This is the source of the Phi Browser Windows client, published under Apache 2.0. It is not a mirror or a snapshot: the Windows builds we publish are built from this repository, from these commits. A fix that lands here lands in the browser people are actually using.

We publish it for a fairly simple reason. A browser sees everything you do, and we would rather you could read the code that runs it than take our word for how it behaves. Developing in the open tends to produce better software too: bugs get found that we would have missed, and people who care about one detail more than we ever could come and fix it properly.

Not all of Phi is here, and we would rather point at the line than let you discover it. The browser engine is our own Chromium build. We release it as a built binary that runs the client, but its source is not published. The server side stays with us as well: the models, the prompts, and the pipelines behind the AI features. Phinomenon Inc. is a company rather than a foundation, and that shapes where the boundary sits. Section 1 of our [Terms of Use](https://phibrowser.com/terms/) sets out the same boundary in legal language.

### A build you make yourself

The Apache licence gives you the right to build this source, change it, and ship it. A build you make is your own browser. It is not the Phi we distribute, and the two are not interchangeable.

A self-built Phi does not sign in to a Phi account, does not reach Phi Cloud, does not sync, and does not use our connectors or messaging relay. It does not receive our updates: the updater is part of what we ship, not part of what we publish. It carries your signature or none.

Our [Terms of Use](https://phibrowser.com/terms/) and [Privacy Statement](https://phibrowser.com/privacy/) cover the application we distribute. They do not cover a build you made.

Do not present a build of your own as the application we distribute.

<br>

## Architecture

Phi on Windows is a **framework + client** split. The Phi framework is our Chromium build, shipped as a binary. `phi_client.dll` is the native Windows client, loaded into the browser process by the framework's `Phi.exe`. It owns everything the user sees: the window chrome, tabs, sidebar, spaces, settings, and the agent surfaces. The UI is native Win32 drawn with Direct2D/DirectWrite, driven by framework → client callbacks, not an embedded web layer. Child processes (renderer, GPU, utility) run the same `Phi.exe` without the client.

The deeper design and build notes live in [`docs/`](docs/) — see `docs/static-build-plan.md` for the build, and `docs/windows-native-ui.md` and `docs/sidebar-architecture.md` for the UI design.

<br>

## Build from source

The client builds on its own, without a Chromium checkout. It is an ordinary Windows DLL, and everything it compiles is in this repository.

### Requirements
- Windows x86_64
- Visual Studio 2022 or later with the "Desktop development with C++" workload
- Python 3

### Steps
From a Visual Studio developer prompt (x64):

```
python tools\fetch-framework.py
python build-client.py
out\Release\Phi.exe --user-data-dir=%TEMP%\phi-dev
```

`fetch-framework.py` downloads the prebuilt framework this commit is pinned to in `framework.lock` and checks its SHA-256. `build-client.py` produces `phi_client.dll` beside it, in a few minutes the first time and seconds after an edit. It uses clang-cl if it finds one and MSVC otherwise. Use a separate `--user-data-dir` so a development build never touches an installed Phi.

The framework is `Phi.exe`, `chrome.dll` and their runtime files. It is prebuilt because building Chromium takes hours and tens of gigabytes, and its source is not published. `client/compat` is the small layer that stands in for the parts of Chromium's `base` the client uses.

<br>

## Contributing

Because the Phi we distribute is built from this repository, a contribution accepted here reaches everyone using Phi. You are not patching a side project or a public mirror; you are changing the browser that ships.

Contributions are welcome: bug reports, feature requests, documentation, and pull requests. Being early, the Windows client has plenty of room for both polish and new ground. Found a bug or have an idea? Open an issue first. To contribute code, send a PR with a clear description of the change and the motivation behind it.

### Getting in touch

- **Discord** for community chat, and for talking to the people who build Phi in something close to real time: [discord.com/invite/aj7rumAMgC](https://discord.com/invite/aj7rumAMgC)
- **X** for development news as it happens: [@phibrowser](https://x.com/phibrowser)
- **Security issues: please tell us privately first, at [hi@phi.cc](mailto:hi@phi.cc).** Not in an issue, not in a PR, not on Discord. Give us the chance to fix it before it is public.

### Translations

Phi ships in several languages, currently machine-seeded and waiting for native speakers to make them feel right. If you can read a JSON file, you can help: every user-visible string in the Phi product family lives in [phibrowser/phi-i18n](https://github.com/phibrowser/phi-i18n), each with context on where it appears and what it does. Translations merged there flow into the next Phi build automatically, credited to you. Prefer a web UI over git? Our Weblate is live at [i18n.phibrowser.com](https://i18n.phibrowser.com): browse every string without an account, sign in to translate, with a shared glossary and context for each one.

<br>

## License

Apache License 2.0. See [LICENSE](LICENSE). The Phi framework (our Chromium build) that runs the client is not covered by that licence; it is our proprietary build, redistributed to you as a binary. Third-party components are listed in the credits reachable from **About Phi**, and the Chromium engine's own components at `chrome://credits`.
