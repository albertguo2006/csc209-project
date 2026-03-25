## CSC209 Assignment 3 Proposal \- TermiNuke (Category 2\)

Team Members: Albert Yanpeng Guo, Kaitlyn Ruixue Zhu, Yibin Wang

### Description

The Battle Arena is a terminal-based multiplayer (2-4) combat simulation game, where players compete in a turn-based battle until one player remains and wins. The game will be created using a TCP client-server application. The server acts as the game engine, managing player statuses (ex. health/HP), turn synchronization, and combat logic.

Players connect via TCP and choose from a list of basic actions: attack, defend/idle, and heal. The game does not progress to the next phase until all players have submitted their choice of action. Each player’s decision process and choices will be hidden until all players finish their selection of actions and the server carries out these actions. The actions will be described on the terminal.

### Architecture

* The Main Event Loop: Processing and monitoring the game  
1) Adding new players: accept new players to the game for a maximum of 4  
2) Carrying out commands: reading and processing the player’s moves  
3) Disconnection: monitor if any player disconnects from the game and act correspondingly (continue game or end game, depending on the conditions)

* State Logic: The server will transfer between 3 states:  
1) Lobby: Gathering the required number of players and waiting for game to start  
2) Action Collection: During the game, collecting all file descriptors to provide a valid action command  
3) Calculation: Process the changes in different players’ status caused by their actions (attack/healing/defend) and display results to all players simultaneously

* Data Management: All information about a player will be stored in a struct “Player”, containing information such as health (HP), action items, connection status, etc.

### Error & Special Case Handling

* Player Disconnects: If a player disconnects from the game, the server will immediately close its file descriptor, remove the player from the players set, and either declare the player as “defeated” and continue or end the game if only one other player is remaining

* Invalid Action Message: If a player inputs an invalid action (e.g., an incomplete message or an action that’s not in the available set), the server will report an error to only that specific player and wait until they have input a valid action

* Player Ties: If two players reach 0 hp at the same time, the server reports a special message indicating a tie.