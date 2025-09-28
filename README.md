<p align="center">
  <img
    src="https://github.com/user-attachments/assets/df4f895f-26e3-4a4b-8bad-1f733cbcda08"
    alt="PolyPlug"
    width="66%"
  />
</p>

PolyPlug Firmware
====================
Welcome to the official GitHub page of the PolyPlug!

PolyPlugs are a open-source smart plugs with **built-in Wi-Fi, Bluetooth, and LoRa**. They also have support for setting schedules, timers, toggles, and modes.

**PolyPlugs were designed to be [used with a PolyCast5](https://github.com/RoboticWorx/PolyCast5)** but being open-source and all, you do you! *(Though, they definitiely work best when controlled with a PolyCast5.)*

You can find some relevant links below:
* [PolyCast5 official website.](https://polycast5.com/) A graphical explanation of what you can do with it and how it works!
* [PolyCast5 user documentation.](https://polycast5.com/pages/docs) How to use it and unleash its full capabilities.
* [PolyCast5 attachment tutorials.](https://polycast5.com/pages/tutorials) Custom builds you can control with your PolyCast5. Including:
  * Home light switcher
  * Door locker/unlocker
  * Button pusher
  * and more!

## Project Structure
* `main` - Initialization code. The program runs from here.
* `build` - Generated program build files. Contains ESP-IDF as well as the compiled code.
* `components` - The various pieces of the application code broken into folders.
  * `common` - Shared macros across the entirety of the program used for build and debugging.
  * `espnow` - Receive instantaneous low-power ESP-NOW commands from a [PolyCast5](https://github.com/RoboticWorx/PolyCast5). Used for syncing network data and protocols, as well as exchanging LoRa encryption keys.
  * `gpio` - Handles broken out GPIOs, when to toggle the built-in 15A relay, and RGB LED indicators.
  * `lora` - Communicates over LoRa to send and receive **lo**ng-**ra**nge signals for communicating with a [PolyCast5](https://github.com/RoboticWorx/PolyCast5).
  * `sx126x` - Driver for the SX1262 LoRa radio chip.
  * `wifi` - Connects to networks, handles wireless updates, and sends data over long distances via MQTT.

## Licensing
*Code in this repository is licensed under [CC Attribution-NonCommercial-ShareAlike 4.0 International](https://github.com/RoboticWorx/PolyPlug/blob/main/LICENSE.md) ([canonical](https://creativecommons.org/licenses/by-nc-sa/4.0/)).*

*Unless required by applicable law or agreed to in writing, this
software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.*