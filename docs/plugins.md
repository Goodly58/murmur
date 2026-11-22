# Plugins

A plugin is another program that follows your session and does something of its own with it, like dimming the lights for a break. Murmur comes with none. The plug button lists yours and opens the folder they go in, `%APPDATA%\Murmur\plugins`.

**Nothing in that folder runs until you switch it on.** A plugin has all of your rights, so only add one you trust.

## Write one

A folder with a `plugin.json` in it:

```json
{
  "name": "My plugin",
  "about": "One line on what it does",
  "run": ["python", "my_plugin.py"],
  "settings": false
}
```

`run` is the program and its arguments. A bare name is looked for in the plugin's folder first, then on PATH. The plugin's folder is the working directory.

Murmur writes one line of JSON to the program's standard input whenever the session changes:

```json
{"event":"hello","murmur":"1.1.0","protocol":1}
{"event":"state","running":true,"phase":"focus","scene":"sprint","study":true,"plan":"p15","round":1,"rounds":4,"remaining":900}
```

| Field | |
|---|---|
| `running` | False while paused, before the first start, and once the plan is over |
| `phase` | `focus`, `break`, `timer` (Calm and Sleep), `free` or `done` |
| `study` | False for Calm and Sleep |
| `round`, `rounds` | Both 0 for a plan without rounds |
| `remaining` | Seconds left of this phase, or `null` when it has no end |

- **Status:** print `{"status":"2 zaps"}` (up to 40 characters) and it shows under the plugin's name.
- **Settings:** with `"settings": true` the plugin gets a settings button, which sends `{"event":"settings"}`.
- **Lifetime:** it starts when a plan starts and should read until its input ends. It may exit whenever it likes and is started again at the next change. When Murmur closes or the plugin is switched off, it has about a second to leave.

Try one without the window:

```
murmur_dev.exe --plugin-try C:\path\to\my-plugin
```
