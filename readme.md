🚀 Multi-Client Chat System Using Linux (USP Mini Project)

A fully functional multi-client chat application built using Unix System Programming (USP) concepts such as:
    Process creation (fork())
    Inter-process communication (pipe())
    Command execution (exec())
    Networking (socket(), bind(), listen(), accept())
    Event multiplexing (select())
    File handling (chat history logs)
    Signal handling
    Process coordination
This project includes a Client, Admin Client, Server, Filter Process, Room Management, Message History, and more.

📌 Features
🧵 Core Chat Features:
    Multi-client support (up to 128 users)
    Room-based chat (/join <room>)
    Username change (/nick <name>)
    Private messaging (/pm <user> <msg>)
    Chat history per room (/history)
    List active rooms (/rooms)
    Clean command-line interface

🛡️ Admin Features:
    Admin has full control with secure password:
    MUTE <user> — Prevents a user from sending messages
    UNMUTE <user> — Restores messaging
    KICK <user> — Disconnects a user
    BROADCAST <msg> — Send a global announcement
    USERS — List all active users
    ROOMS — View all active rooms
    Receives live appeals from muted users

📂 Message Logging:
    Each room has its own log file
    Stored under logs/roomname.log
    Used for history retrieval

🧹 Profanity Filter:
    Offensive words sanitized using a separate filter process executed via:
        fork() → exec() → filter

🏗️ Project Structure:
    multi-chat/
    │── src/
    │   ├── server.c
    │   ├── client.c
    │   ├── admin_client.c
    │   └── filter.c
    │
    │── logs/
    │── reports/
    │── Makefile
    │── README.md

⚙️ Build Instructions:
    Make sure you're on Linux / WSL / Ubuntu:
        sudo apt update
        sudo apt install build-essential

    Compile everything:
        make

    Clean binaries:
        make clean

▶️ How to Run:
1️⃣ Start the Server
./server

2️⃣ Start a Client
./client <server-ip>

Example:
./client 127.0.0.1

3️⃣ Start the Admin Client
./admin_client <server-ip>

Admin password is set inside server.c:
#define ADMIN_PASSWORD "admin123"

🧑‍💻 Client Commands
    Command	                    Description
    /nick <name>	            Change username
    /join <room>	            Switch rooms
    /rooms	                    List all active rooms
    /history	                View room chat history
    /pm <user> <msg>	        Private message
    /appeal <msg>	            Appeal to admin when muted
    /quit	                    Exit client

👑 Admin Commands
    Command	                        Function
    MUTE <user>	                    Mute a user
    UNMUTE <user>	                Unmute a user
    KICK <user>	                    Disconnect user
    USERS	                        List all connected users
    ROOMS	                        List all active rooms
    BROADCAST <msg>	                Global announcement
    QUIT	                        Exit admin client

📜 How Message Filtering Works
    Each time a client sends a message:
    server fork() → exec() → ./filter

The message is transmitted through pipes:
    Pipe 1: Parent → Filter (raw message)
    Pipe 2: Filter → Parent (cleaned message)
This ensures separation of filtering logic from server logic.

🧪 Testing
Typical testing setup:
    1 server terminal
    2–5 client terminals
    1 admin terminal

Test cases include:
    Room switching
    Private messages
    Muting/unmuting
    Broadcasting
    Client disconnects
    Profanity filtering
    History loading
    Admin appeal flow

🛠️ Future Enhancements
    GUI-based client (GTK/QT)
    Encrypted communication (TLS)
    Multi-admin support
    Database-backed chat logs
    Websocket-based front-end
    Load-balanced server cluster

📄 Credits
    Developed as part of BCS515C – Unix System Programming mini project.

📁 License
    This project is open-source and free to use for educational purposes.