# Cyphercon9 Badge Firmware

MicroPython firmware for the Cyphercon9 conference badge, running on a Raspberry Pi Pico with a 132x32 LCD display, 6 red LEDs, buttons, and a vibration motor.

## Updating the Badge

Connect the badge to your computer via USB. The badge exposes a serial REPL (no mass storage).

**Install mpremote** (one time):

```bash
pip install mpremote
```

**Transfer firmware:**

```bash
mpremote connect /dev/cu.usbmodem101 cp blue-badge.py :main.py
```

**Monitor debug output:**

```bash
screen /dev/cu.usbmodem101
```

> Note: Only one program can use the serial port at a time. Quit `screen` (Ctrl-A, K) before using `mpremote`.

## Features

### Hardware

- **Display**: 132x32 pixel LCD (SPI) with character/glyph rendering
- **LEDs**: 7 total (1 Pico onboard LED + 6 external red LEDs) with rotation, pulse, and chase animations
- **Buttons**: Social button + two 3-way switch groups (up/down/push each)
- **IR**: 38kHz IR transmitter via PIO state machine
- **Vibration motor**: For notifications

### Modes

| Mode | Description |
|------|-------------|
| **Idle** | Home screen with scrolling text, random animations, and LED light show. Shows mail indicator when unread messages exist. |
| **Alias** | Edit your 16-character display name. Cycle through glyphs with up/down, move cursor with left/right, shift to toggle character set. |
| **Inbox** | Browse up to 32 received messages. Navigate with up/down, mark as read with button press. |
| **Compose (Page 1)** | Select a recipient from known contacts. |
| **Compose (Page 2)** | Write a 16-character message using glyph selection. Send with the social button. |

### Radio / Networking

- **Direct messages** (pages) and **broadcasts** relayed by other badges
- Checksum and sync word validation on all packets
- Echo detection to ignore your own retransmitted packets
- Queue system for pending transmissions
- Badge hierarchy based on serial number ranges (founder, extreme, lifetime, speaker, general, vendor) with different relay permissions

### Persistent Storage

- **serial**: Unique 2-byte badge identifier
- **alias_memory**: Contact names (676 entries x 16 bytes)
- **social_memory**: Tracks which badges you've encountered
- **inbox_memory**: Received messages with unread tracking

## CLI Tool (`badge-cli.py`)

An interactive command-line tool for controlling the badge over USB serial from your computer.

### Setup

```bash
pip install pyserial
```

### Usage

```bash
# Interactive mode (auto-detects serial port)
python badge-cli.py

# Specify port manually
python badge-cli.py -p /dev/cu.usbmodem101

# Single command mode (for scripting)
python badge-cli.py -c "BC:hello world"
```

### Commands

| Command | Description |
|---------|-------------|
| `broadcast <message>` | Send a broadcast message over IR (16 chars max) |
| `page <badge_id> <message>` | Send a direct message to a specific badge |
| `inbox` | List all inbox messages (shows read/unread status and type) |
| `mark-read` | Mark all inbox messages as read |
| `status` | Show badge serial number, type, alias, and unread count |
| `alias` | Show current alias |
| `alias <name>` | Set a new alias (16 chars max) |
| `idle <top> \| <bottom>` | Set custom idle display text (use `\|` to separate top and bottom lines) |
| `reset-idle` | Reset idle display to default animation |
| `help` | Show available commands |
| `quit` | Exit the CLI |

### Custom Idle Display

When a custom idle message is set, the top line shows your text and the bottom line shows the second text. An up-arrow animation sweeps across the top row whenever the badge has messages queued for transmission.

Reset to the default scrolling animation and Cyphercon9 logo with `reset-idle`.

### Notes

- The CLI auto-detects the badge's USB serial port (looks for `usbmodem` or `acm` devices)
- Debug output from the badge is shown with a `[debug]` prefix
- Only one program can use the serial port at a time — quit `screen` before using the CLI

## Changes in This Branch (`input_refactor`)

### Compose Cursor Remembers Previous Letter

When composing a message and moving to the next character position, the cursor now starts at the same letter you previously selected instead of resetting to the beginning of the character list. This makes typing words with repeated or nearby letters much faster.

### Idle LED Light Show

When the badge is idle with no unread messages, the LEDs perform a repeating light show:

1. **Clockwise chase** — a single LED chases around the ring (12 steps, ~1.8s)
2. **Counter-clockwise chase** — reverses direction (12 steps, ~1.8s)
3. **Off** — all LEDs turn off for 20 seconds
4. **Repeat**

The mail notification (LED rotate) still takes priority when unread messages exist. The light show is distinct from the mail indicator so you can tell them apart at a glance.

### Auto-Greeting (New)

The badge now has a "passive" interaction mode. When it receives a packet from another badge, it will automatically respond with a broadcast greeting (`Greetz from Cy9!`) if it hasn't greeted that specific badge ID in the last 5 minutes. This feature helps you discover other badges at the conference without manual interaction.

## Flipper Zero Application

A custom Flipper Zero app (`cy9_greet`) is included to interact with the badge's IR protocol. It allows you to broadcast a greeting from your Flipper Zero, which will appear on nearby badges as being from your Flipper's unique name (e.g., `M3m0ry`).

### Compiling and Installing

1. **Install ufbt** (in your Python venv):
   ```bash
   pip install ufbt
   ```
2. **Build and Launch**:
   Connect your Flipper Zero via USB and run:
   ```bash
   cd flipper-app
   ufbt launch
   ```
3. **Manual Install**:
   If `ufbt launch` fails to find your device, you can manually copy the compiled `.fap` file:
   - File location: `flipper-app/.build/f7-firmware-D/cy9_greet.fap`
   - Destination: Flipper SD Card `/apps/Infrared/cy9_greet.fap`

### Using the App
Open **Apps > Infrared > Cy9 Greeter** on your Flipper Zero. Point it at a badge and press **OK** to send the greeting.
