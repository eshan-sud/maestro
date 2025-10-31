# Maestro

**Maestro** is a **Command-Line Music Player Daemon & Client** — a socket-based client–server application where the **server** manages a playlist of audio files and controls playback using an external player like `mpg123`, while the **client** connects to send commands such as `play`, `pause`, `next`, `add`, and `quit`.

The project demonstrates **process management**, **IPC (Inter-Process Communication)**, and **socket programming** concepts in C — using `fork()`, `exec()`, `pipes`, and `select()` to create a robust terminal music player with a real-time status display.

## Sample Output (Client)
<img width="500" height="309" alt="Play command" src="https://github.com/user-attachments/assets/67136af3-b474-42cb-bd45-b5580c806211" />
<img width="500" height="328" alt="Pause command" src="https://github.com/user-attachments/assets/46f320fa-f3ea-4601-b482-b337cd427c99" />
<img width="500" height="339" alt="Next command" src="https://github.com/user-attachments/assets/fa49a96d-9bff-4c11-b8aa-2d07cbbe5146" />
<img width="500" height="364" alt="Stop command" src="https://github.com/user-attachments/assets/83608a18-e680-485a-b6ea-a2929767f83e" />

## Sample Output (Server)
<img width="500" height="159" alt="Screenshot 2025-10-31 115355" src="https://github.com/user-attachments/assets/84c380db-4fcc-4071-8801-d2ae6e9f0ae0" />

## Sample Output (logs.txt)
<img width="500" height="181" alt="Screenshot 2025-10-31 115437" src="https://github.com/user-attachments/assets/3f681bcd-f57d-495a-ad29-a8386d9c87b4" />


## Architecture Overview

- **Server (`server.c`)**

  - Manages playlist, playback, and song state.
  - Spawns a child process via `fork()` to control `mpg123`.
  - Handles multiple client connections via TCP sockets.
  - Periodically sends updates:
    - `STATUS` → Current playback state (`PLAYING`, `PAUSED`, `STOPPED`)
    - `PLAYING` → Currently playing song name
    - `NEXT` → Next song in the queue

- **Client (`client.c`)**
  - Connects to the server and provides an interactive CLI.
  - Sends user commands (`play`, `pause`, `next`, `add path`, etc.).
  - Displays a dynamic UI with:
    - Current song name
    - Next song in queue
    - Real-time progress bar and elapsed time

---

## Technical Stack

| Component       | Technology                 |
| --------------- | -------------------------- |
| Language        | C                          |
| Playback Engine | `mpg123` (via fork & exec) |
| IPC Mechanism   | Pipes & Signals            |
| Networking      | TCP Sockets                |
| CLI Rendering   | Ncurses                    |
| OS              | Linux (Ubuntu / WSL2)      |

## Start the application

### 1. Install dependencies

```bash
sudo apt update
sudo apt install build-essential git mpg123 ffmpeg libncurses5-dev libncursesw5-dev -y
```

### 2. Compile

```bash
cd maestro
make clean && make
```

### 3. Prepare songs

Add sample .mp3 files into the songs/ directory & write the respective paths in the `playlist.txt`

Example:

- songs/Free_Test_Data_2MB_MP3.mp3
- songs/Free_Test_Data_1MB_MP3.mp3

### 4. Run the application

Start the server:

```bash
./server
```

Run the client in another terminal:

```bash
./client
```

## Supported Commands

| Command                 | Description                   |
| ----------------------- | ----------------------------- |
| `play`                  | Starts or resumes playback    |
| `pause`                 | Pauses current song           |
| `next`                  | Skips to the next song        |
| `add /path/to/song.mp3` | Adds new song to the playlist |
| `exit`                  | Exits client gracefully       |

## Features Implemented

| Category                        | Feature                                            | Status |
| ------------------------------- | -------------------------------------------------- | ------ |
| **Environment Setup**           | Linux env (WSL2), tools installed, repo structured | ✅     |
| **External Player Integration** | `fork()` + `exec()` + pipe control for mpg123      | ✅     |
| **Playlist Management**         | Add, Next/Prev, Track current song                 | ✅     |
| **Command Parsing**             | `play`, `pause`, `next`, `add`, error handling     | ✅     |
| **Client-Server Communication** | TCP sockets for commands and updates               | ✅     |
| **CLI Interface**               | Ncurses-style UI with progress bar & playback info | ✅     |
| **Real-time Updates**           | Current song, next song, progress tracking         | ✅     |
| **Signal Handling**             | Graceful termination with cleanup                  | ✅     |

## Future Additions

- Visual Enhancements: CLI-based waveform or “Now Playing” ASCII art
- Colourise UI: Eg, Green for "Playing", Yellow for "Paused", Blue for progress bar, Red for errors/logs
- Multiple Playlists: Manage different song lists & save queues
- Save queue as separate playlist
- Persistent Storage: Save & reload previous sessions or custom playlists
- Recommendation System: Hybrid model combining Collaborative & Content-based filtering
- GUI version: Desktop client using GTK or Qt
