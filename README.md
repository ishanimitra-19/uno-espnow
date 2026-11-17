# Wireless UNO

UNO for 2–3 players on LILYGO T-Display boards over ESP-NOW.

Upload `graphuno.ino` as its own Arduino sketch with TFT_eSPI set up for your board. Use the same `TEAM_ID` on every board and a different `PLAYER_ID` (0–2) for each player.

Left cycles through cards or colors. Right plays, draws, or confirms a color. Pressing both buttons resets to a new game. 

The logic of the game is in the backend, and the Figma UI design is the frontend. Both front and backend are in one file, in this github called graphuno.ino 

# Set Up

Connect three ESP 32 microcontrollers via USB-C to three different computers. 
Open Arduino IDE, and upload the file called graphuno.ino, compile the sketch. Make sure BAUD is set to 115200. When clicking a device, make sure it is set to ESP 32 Dev Module. Then compile. 

The LED of the ESP32 should now be open to the game of UNO. Have fun! 

[UNO rules](https://www.unorules.com/)

<img src="images/uno-game.jpg" alt="UNO game in progress on a LILYGO T-Display" width="300">
