# PaperBit OS — User Manual

> **Manual v1.1** · for PaperBit firmware 1.4.16+ (Xteink X3 / X4)

Welcome to PaperBit OS, the reading and note-taking system for your
Xteink e-ink reader. This manual lives on your device, so you can read
it here any time.

While reading, watch the **button hints** along the bottom of the
screen — they always show what each button does right now. When in
doubt, read the hints.

## Getting started

### Buttons

PaperBit uses your device's physical buttons:

- **Back** — go back, or return to the Home screen.
- **Open / Select** — open the highlighted item, or confirm.
- **Up / Down** — move the highlight in a list; **turn pages** while
  reading. Hold to move faster.

The bottom hints label each button for the screen you're on. Buttons
can be remapped in **Settings**.

### Power, sleep and battery

- The battery level shows in the top corner of most screens.
- After a period of no activity the device **sleeps** and shows a
  sleep image. Press any button to wake it.
- Sleep timing and the power button's behavior are set in
  **Settings**.

### The Home screen

Home is your starting point. From here you can reach:

- **Continue Reading** — jump back into your most recent book (shown
  when you have one).
- **Browse Files** — the file browser for everything on the card.
- **Recent Books** — anything you've opened lately.
- **File transfer** — wireless transfer tools.
- **Paperbit Fetch** — pull documents over Wi-Fi.
- **Notes** — write with a Bluetooth keyboard.
- **My Vault** — your PIN-protected private notes.
- **Settings** — reading, display, buttons, Wi-Fi, and more.

Highlight an item and press **Open** to enter it.

## Reading

Open any supported file and PaperBit renders it right on the device.
Your reading position is saved automatically and restored when you
reopen a document.

Supported formats:

- **EPUB** — books with chapters, styles, and images.
- **Markdown** (*.md*) — rendered natively (see the next section).
- **Plain text** (*.txt*).
- **XTC** — pre-prepared page format.

The screen shows **four levels of gray**, so photos and shaded art
have real depth, not just black and white.

**Images:** pictures are auto-fit to the page. If an image is too big
to draw safely, the device shows a small **[image too large]** marker
in its place instead of stalling — the rest of the page still reads
normally.

While reading, the status bar shows the title and your progress
(page and percent). Page turns, font, spacing, and margins are all
adjustable in **Settings**.

## Reading Markdown

Markdown documents (*.md*) render directly on the device — no
computer and no conversion. This is the same format your Notes use,
so any note is also a readable document.

### What renders

- **Headings** show bold and centered. Every `#` and `##` heading
  also becomes a Table of Contents entry.
- **Bold**, *italic*, and ***bold italic*** text.
- ~~Strikethrough~~ text.
- Bulleted and numbered lists.
- Links — the link text shows, with its address noted on the page.
- Paragraphs wrap and flow like a book.

Good to know about the reader's limits:

- Headings are bold and centered but stay at normal text size — there
  is no larger heading size.
- `Inline code` shows in italics (there is no typewriter font).
- Task-list boxes are shown as plain characters — you'll see the
  literal `[ ]` or `[x]` brackets rather than a tappable checkbox.

### Table of contents

While reading a Markdown document, press **Open** to bring up
**Contents** — a list of the document's `#` and `##` headings, with
sub-headings indented. Highlight a heading and press **Open** to jump
straight to it, or **Back** to return. If a document has no headings,
a brief "No headings" note appears instead.

## Paperbit Fetch

**Paperbit Fetch** pulls documents onto your device over Wi-Fi — no
cables. It's ideal for things you update often, like a shopping list
or a daily digest.

### Setting the source

Fetch pulls from a folder address (a URL) that you control. The
easiest way to set it is from the **Paperbit desktop app** on your
computer — it saves the address to your device over USB.

You can also set it on the device: open **Paperbit Fetch**, choose
**Set source URL**, and type the folder's address.

### Using it

1. Open **Paperbit Fetch** from Home.
2. The first time, pick your Wi-Fi network when asked.
3. The device lists the documents in your folder.
4. Highlight one and press **Open** — it downloads and opens right
   away.

Your folder just needs the document files in it (*.md*, *.epub*,
*.txt*, or *.xtc*). The device lists whatever it finds. If you'd
rather curate the list and give documents friendly names, the folder
can include an index file — but that's optional.

### If Fetch can't load

Fetch always tells you what went wrong and offers **Retry** or
**Set URL**:

- **Wi-Fi connection failed** — couldn't join the network. Try again
  or check the password.
- **Can't reach the source** — on Wi-Fi, but the folder couldn't be
  opened. Check your internet and the address.
- **Source not found (404)** — nothing to list at that address. Point
  it at the folder that holds your documents.
- **No documents found** — the folder was reached but is empty. Add
  some files.

## Notes

**Notes** turns your reader into a distraction-free typewriter. Pair a
Bluetooth keyboard once, then write plain Markdown notes that save
themselves as you type.

Notes needs a **Bluetooth LE (BLE) keyboard**. Older Bluetooth
"Classic" keyboards and USB-dongle keyboards are not supported and
won't appear in the list.

### Pairing a keyboard

1. Go to **Settings → Keyboard**.
2. Put your keyboard into **pairing mode** (check its manual).
3. Choose **Scan for keyboard** and wait a few seconds.
4. Select your keyboard from the list to pair it.

You only pair once — after that the device reconnects to it
automatically whenever you open Notes. To remove it, return to
**Settings → Keyboard** and choose **Forget keyboard**.

### Writing a note

1. From Home, open **Notes**.
2. Choose **+ New note**, or highlight an existing note and press
   **Open**.
3. Start typing. It works like a typewriter: text is added at the
   end, **Backspace** deletes, and **Enter** starts a new line.
4. There is no Save button — your note **saves continuously** as you
   type.
5. Press **« Exit** when you're done.

The status line at the top shows the keyboard's connection. If it
says no keyboard is paired, pair one in **Settings → Keyboard** first.

### Where notes live

- Notes are ordinary **Markdown files**, kept in the **Notes folder**
  on your card.
- In the Notes list, each note is titled by its **first `#` heading**.
  If a note has no heading, its filename is used. Start a note with a
  line like `# Groceries` to give it a clear title.
- Because notes are Markdown, you can also open them from **Browse
  Files** and read them in the Markdown reader.
- To delete a note, **hold Open** on it in the Notes list and confirm.

### Wi-Fi pauses while you type

Your device shares one radio between Bluetooth and Wi-Fi, so it can
only use one at a time. While you're in Notes with the keyboard
connected, **Wi-Fi and Paperbit Fetch are paused**. When you leave
Notes, Wi-Fi becomes available again. This is normal — nothing is
lost.

## My Vault

**My Vault** keeps private notes locked behind a PIN. Everything in
the vault is **encrypted** on the card, so it stays unreadable
without your PIN.

- The **first time** you open My Vault, you set a vault PIN. After
  that, you enter your PIN to unlock it.
- **+ New note** — type a note on the on-screen keyboard; it's
  encrypted and saved when you finish.
- **+ Import .md from SD** — pick an existing Markdown file on your
  card. It's moved into the vault (encrypted), and the plain,
  unencrypted original is deleted so no readable copy is left behind.
- Highlight any vault entry and press **Open** to read it.
- Press **Back** to leave — the vault locks again immediately.

## Files

**Browse Files** (from Home) opens the file browser. It lists the
folders and documents on your card, and you can move into any folder
to find what you want.

Where things live:

- Your writing is in the **Notes folder**.
- Books can live anywhere; the desktop app puts them in the **Books
  folder** by convention.
- Documents you pull with Paperbit Fetch land in the **Fetch folder**.

To open a document, highlight it and press **Open**. To delete a
file, **hold Open** on it and confirm.

## Settings

Open **Settings** from Home to tune your reader:

- **Reading** — font, size, line and paragraph spacing, alignment.
- **Display** — theme, orientation, margins, and refresh.
- **Buttons and power** — remap buttons, power-button behavior, and
  the auto-sleep timeout.
- **Wi-Fi** — join a network.
- **Keyboard** — pair or forget a Bluetooth keyboard for Notes.

## Updating the firmware

There are two ways to update PaperBit OS.

**Wirelessly, right on the device:** open **Settings → Check for
updates**. The reader connects to your saved Wi-Fi network and looks
for a newer version. If one is available, confirm it — the device
restarts, shows **"Installing update..."** with progress while it
downloads and installs, then starts up on the new version. If you are
already up to date it simply says **"No update available."** Keep the
device powered during an update; if anything goes wrong it says so and
starts normally on your current version.

**From your computer:** connect over USB, open the **Paperbit desktop
app**, and use **Utilities → Firmware** to check for and install the
latest version. The app checks each update before it installs.

Either way, a previous version stays available as a safe fallback.
