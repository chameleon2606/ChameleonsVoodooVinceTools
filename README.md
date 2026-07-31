# Chameleon's Voodoo Vince Tools

<img width="802" height="632" alt="image" src="https://github.com/user-attachments/assets/ad928f16-f2d3-41d6-a960-644b8ae944c3" />

#### Extractor:
* This is a program that lets you extract 3D models, textures, sounds and other game data
* the game uses .dds files as textures. the extractor automatically converts them into .png files
    
#### 3D Models:
* when extracting 3D models, the relevant textures are being extracted as well
* the 3D models are converted from their proprietary .gator format into the widely usable glTF format
* it's optionally possible to pack all the glTF data into a single .glb file
* all rigging data is included for every model, but they can be disabled (for slightly smaller file sizes) as well
* some models have more complex collision data. These get exported as simple .obj models

#### Level extraction:
* extracting the world.hot file will use the containing files and build a .obj file of that level, including textures
* these will only contain all static elements of the level
* lightmaps are not yet included

#### Repacker:
* Included is a tool to repack custom textures and sound effects back into the game
* the download contains a folder with every texture and sound from the game
* replace the desired texture and/or sound in the folder and click _**'repack'**_
* the textures **must** be the exact same **name**, **format** (DXT1 .dds with mipmaps) and **size** as the original texture.
* if you're unsure how to create these .dds files, there is an [adobe photoshop plugin by NVIDIA](https://developer.nvidia.com/texture-tools-exporter) that lets you work with .DDS files<br>
just choose the format **8.8.8.8 BGRA 32 bpp**

<img width="802" height="632" alt="Repacker" src="https://github.com/user-attachments/assets/9ea2837d-d1c9-4d90-a5a1-491576790f3c" />
