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
* Test all changes and prove they work and fufill the stated goals and acceptance criteria
* Take before and after screenshots to confirm that changes were made and done correctly

# Coding Standards
* Follow clean code principles
* Components should be loosely coupled

# General Rules
* Do not read any files in ./ignore
* Never commit or push anything to git
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
#ffaa00 Buttons, Tabs, and Header background
#ffcc00 Hilight
#ff9900 Darken

#000000 Background
#313131 Card background
#454545 Text field
#818181 Text field hilight
