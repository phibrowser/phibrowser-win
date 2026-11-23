![Phi Browser](docs/assets/phibrowser_github_2000.png)

# Phi Browser for Windows

**A crafted front door to the internet.**

*Simple until asked for more, made for humans and whichever AI tools they use, and local first so data and privacy stay where they belong.*

Phi Browser for Windows is a Chromium-based browser built as a real native Windows app (Win32 + DirectWrite). It is a clean everyday browser first, with the power tools one reach away. AI is welcome and never required: AI agents can work in it beside you, what it remembers stays in a file on your PC that you can read, and the models can run entirely on your own machine.

> **Early access.** The Windows client is young. The core browser is solid, some features are still landing, and the macOS version is further along. If something feels off, please tell us.

**Download Phi Browser for Windows:** early-access builds are on the [Releases](../../releases) page. For macOS, see [**phibrowser.com**](https://phibrowser.com).  
Like it? A ⭐ helps a lot.

<br>

## Why Phi Browser

- **Simple.** It opens clean and fast. Nothing in your way.
- **Powerful when you ask.** Spaces, profiles, split views, tab groups, URL rules, shortcuts, picture-in-picture, incognito spaces. All there. None of it in your face.
- **A real Windows app.** The window, tabs, sidebar and spaces are native Windows UI, not web views, on a real Chromium engine.
- **AI is optional.** On or off, your call. It is a great browser either way.
- **Any AI is welcome.** Use ours. Run your own models on your own machine with LM Studio or Ollama, no account needed. Or let the AI tools you already use drive Phi Browser for you.
- **Your data stays home.** Its memory lives on your device, in a file you can read, edit or wipe. We don't collect it. We don't train on it.
- **No lock-in.** No ad business. No search business. We win only if you like the browser.
- **Open source.** The Windows builds we publish are built from this repo. Read it, build it, fix it.

<br>

## What this repository is

This is the source of the Phi Browser Windows client, published under Apache 2.0. It is not a mirror or a snapshot: the Windows builds we publish are built from this repository, from these commits. A fix that lands here lands in the browser people are actually using.

We publish it for a fairly simple reason. A browser sees everything you do, and we would rather you could read the code that runs it than take our word for how it behaves. Developing in the open tends to produce better software too: bugs get found that we would have missed, and people who care about one detail more than we ever could come and fix it properly.

Not all of Phi Browser is here, and we would rather point at the line than let you discover it. The browser engine is our own Chromium build. We release it as a built binary that runs the client, but its source is not published. The server side stays with us as well: the models, the prompts, and the pipelines behind the AI features. Phinomenon Inc. is a company rather than a foundation, and that shapes where the boundary sits. Section 1 of our [Terms of Use](https://phibrowser.com/terms/) sets out the same boundary in legal language.

### A build you make yourself

The Apache licence gives you the right to build this source, change it, and ship it. A build you make is your own browser. It is not the Phi Browser we distribute, and the two are not interchangeable.

A self-built Phi Browser does not sign in to a Phi account, does not reach Phi Cloud, does not sync, and does not use our connectors or messaging relay. It does not receive our updates: the updater is part of what we ship, not part of what we publish. It carries your signature or none.

Our [Terms of Use](https://phibrowser.com/terms/) and [Privacy Statement](https://phibrowser.com/privacy/) cover the application we distribute. They do not cover a build you made.

Do not present a build of your own as the application we distribute.

<br>

## Architecture

Phi Browser on Windows is a **framework + client** split. The Phi Browser framework is our Chromium build, shipped as a binary. `phi_client.dll` is the native Windows client, loaded into the browser process by the framework's `Phi.exe`. It owns everything the user sees: the window chrome, tabs, sidebar, spaces, settings, and the agent surfaces. The UI is native Win32 drawn with Direct2D/DirectWrite, driven by framework → client callbacks, not an embedded web layer. Child processes (renderer, GPU, utility) run the same `Phi.exe` without the client.

The deeper design and build notes live in [`docs/`](docs/): see `docs/static-build-plan.md` for the build, and `docs/windows-native-ui.md` and `docs/sidebar-architecture.md` for the UI design.

<br>

## Build from source

The client builds on its own, without a Chromium checkout. It is an ordinary Windows DLL, and everything it compiles is in this repository and its one submodule, `phi-client-core/`, which is [phibrowser/phibrowser-client-core](https://github.com/phibrowser/phibrowser-client-core): the code the Windows and Linux clients share (the sync engine, the account's local store, SQLite and BoringSSL). Clone with `git clone --recurse-submodules`, or run `git submodule update --init` in an existing clone.

Build from `main`. It is the released code: it moves only when we publish a release, and its `framework.lock` always names a published framework. Day-to-day work lands on `dev`, which can run ahead of every published framework, so a build of `dev` may not start. A fresh clone starts on `dev`; run `git switch main` first.

### Requirements
- Windows x86_64
- Visual Studio 2022 or later with the "Desktop development with C++" workload
- Python 3

### Steps
From a Visual Studio developer prompt (x64):

```
git submodule update --init
python tools\fetch-framework.py
python build-client.py
out\Release\Phi.exe --user-data-dir=%TEMP%\phi-dev
```

`fetch-framework.py` downloads the prebuilt framework this commit is pinned to in `framework.lock` and checks its SHA-256. `build-client.py` produces `phi_client.dll` beside it, in a few minutes the first time and seconds after an edit. It uses clang-cl if it finds one and MSVC otherwise. Use a separate `--user-data-dir` so a development build never touches an installed Phi Browser.

The framework is `Phi.exe`, `chrome.dll` and their runtime files. It is prebuilt because building Chromium takes hours and tens of gigabytes, and its source is not published. `client/compat` is the small layer that stands in for the parts of Chromium's `base` the client uses.

<br>

## Contributing

Because the Phi Browser we distribute is built from this repository, a contribution accepted here reaches everyone using Phi Browser. You are not patching a side project or a public mirror; you are changing the browser that ships.

Contributions are welcome: bug reports, feature requests, documentation, and pull requests. Being early, the Windows client has plenty of room for both polish and new ground. Found a bug or have an idea? Open an issue first. To contribute code, make and test the change on `main`, then open a PR against `dev` with a clear description of the change and the motivation behind it. `dev` is the only branch we merge into, and your change reaches `main` with the next release. A change to the sync engine or the local store is a PR to [phibrowser-client-core](https://github.com/phibrowser/phibrowser-client-core) (its `main`); once it is merged there, a commit on `dev` here moves the submodule to it.

### Getting in touch

- **Discord** for community chat, and for talking to the people who build Phi Browser in something close to real time: [discord.com/invite/aj7rumAMgC](https://discord.com/invite/aj7rumAMgC)
- **X** for development news as it happens: [@phibrowser](https://x.com/phibrowser)
- **Security issues: please tell us privately first, at [hi@phi.cc](mailto:hi@phi.cc).** Not in an issue, not in a PR, not on Discord. Give us the chance to fix it before it is public.

### Translations

Phi Browser ships in several languages, currently machine-seeded and waiting for native speakers to make them feel right. If you can read a JSON file, you can help: every user-visible string in the Phi product family lives in [phibrowser/phi-i18n](https://github.com/phibrowser/phi-i18n), each with context on where it appears and what it does. Translations merged there flow into the next Phi Browser build automatically, credited to you. Prefer a web UI over git? Our Weblate is live at [i18n.phibrowser.com](https://i18n.phibrowser.com): browse every string without an account, sign in to translate, with a shared glossary and context for each one.

<br>

## License

Apache License 2.0. See [LICENSE](LICENSE). The Phi Browser framework (our Chromium build) that runs the client is not covered by that licence; it is our proprietary build, redistributed to you as a binary. Third-party components are listed in the credits reachable from **About Phi**, and the Chromium engine's own components at `chrome://credits`.
