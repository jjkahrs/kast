Kast is a game engine focused on making it easy to build MMORPGs.

# Tech Stack
* Language: C++20
* Window/input: SDL3
* Renderer: bgfx
* Editor UI: Dear ImGui docking + ImGuizmo
* ECS: EnTT
* Physics: Jolt
* Scripting: LuaJIT + sol2
* CSS Game GUI: RmlUi
* Model import: Assimp (glTF primary)
* Textures:  stb_image + bimg (bgfx companion, KTX/DDS)
* Serialization (scenes/prefabs):  nlohmann/json, binary later
* FSM / Event Bus: own small code on EnTT (entt::dispatcher covers event bus)

# Process
* Create a branch to do work in
* Test all changes and prove they work and fufill the stated goals and acceptance criteria
* Take before and after screenshots to confirm that changes were made and done correctly
* Ask which of these test domains should be run and exclude the others: Editor, Auth, Admin, World, starter-mmo

# Coding Standards
* Follow clean code principles
* Components should be loosely coupled

# General Rules
* Do not read any files in ./ignore
* Never push anything to git
* Tests that open windows should not do it on the main monitor.

# Project Organization
* The game engine editor is under ./editor
* The game services are under ./services

# Kast Editor
* Unity-like game engine editor to enable others to build MMORPGs
* Shipped client: Windows desktop only 
* Support ECS
* Has finite state machine framework
* Has event bus for communication between components
* 3D graphics
* Shaders
* Physics engine
* Lua scripting for game logic
* Entity heirarchy
* Game GUI builder that is based on CSS
* Import 3D models
* Create materials
* Import and use textures
* Prefab support

# Kast Services
* Authentication Server
* World Server

# Editor GUI Color Palette
Defined once in editor/ui_common.h; panels use those constants, never their own colors.
#ffa100 Accent: buttons, check marks, slider grabs, plot lines; text on active tabs, selected items and headers
#FFD000 Highlight: button hover, nav cursor, drag-drop target, text links, viewport labels and gizmo
#e08d00 Pressed: button active, resize grip active
#ffb703 Warning: Console [warn] lines
#e63946 Error: Console [error] lines and all error text

#f3f4f6 Text
#9e9ea8 Secondary text: disabled text, inactive tab text
#0c0c0e Background: windows, menu bar, scrollbar, tab bar, window clear color
#151518 Card: cards, popups, table headers, inactive tabs
#222227 Text field: fields, borders, separators, scrollbar grab, active tab, selected items, header bars
#1c1c20 Row hover: hovered tree row, selectable, header, inactive tab
#3a3a41 Text field highlight: hovered/active field, scrollbar grab hovered, text selection

* Yellow fills only for buttons and interactive controls.
* Tabs, selected items and headers are grey (#222227) with #ffa100 text.
* Text on yellow fills is black.

# Documentation
* Top level README.md should contain discription of the project and how to build and start the editor and services
* Editor documentation should be HTML/CSS under ./docs/editor
* Engine documentation should be HTML/CSS under ./docs/engine
* Auth documentation should be HTML/CSS under ./docs/services/auth
* World server documentation should be HTML/CSS under ./docs/services/world
* All tool documentation should be under the ./tools subdirectory the tool is in.

