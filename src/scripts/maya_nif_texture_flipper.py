"""
Maya script to setup texture flipping from NIF files exported via FBX.

This script reads custom FBX properties added during NIF->FBX export and creates
animated texture sequences in Maya.

Usage:
1. Import the FBX file into Maya
    2. Run this script: 
     import maya_nif_texture_flipper
       maya_nif_texture_flipper.setup_texture_flipping()
"""

import maya.cmds as cmds
import maya.mel as mel

def setup_texture_flipping():
    """
    Finds all materials with NifTextureFlip properties and sets up animated texture sequences.
    """
    # Get all materials in the scene
    materials = cmds.ls(type='phong') + cmds.ls(type='lambert') + cmds.ls(type('blinn'))
    
    flipped_count = 0
    
    for material in materials:
        # Check if material has our custom properties
     if not cmds.attributeQuery('NifTextureFlipFrames', node=material, exists=True):
            continue
       
        # Get flipper properties
     try:
       start_time = cmds.getAttr(material + '.NifTextureFlipStartTime')
       rate = cmds.getAttr(material + '.NifTextureFlipRate')
            num_frames = cmds.getAttr(material + '.NifTextureFlipFrames')
        except:
          print("Warning: Could not read texture flipper properties from " + material)
   continue
        
        print("Setting up texture flipper for material: " + material)
        print("  Start Time: {}, Rate: {}, Frames: {}".format(start_time, rate, num_frames))
        
        # Get the color (diffuse) texture connection
        connections = cmds.listConnections(material + '.color', source=True, destination=False)
        if not connections:
            print("  Warning: No texture connected to color channel")
 continue
        
        file_node = connections[0]
        if cmds.nodeType(file_node) != 'file':
  print("  Warning: Color connection is not a file node")
            continue
    
        # Get the current texture path
        texture_path = cmds.getAttr(file_node + '.fileTextureName')
   
        # Extract base path and construct frame sequence
        import os
        base_dir = os.path.dirname(texture_path)
        base_name = os.path.splitext(os.path.basename(texture_path))[0]
        
   # Maya expects frame sequences in the format: name.####.ext
        # We need to rename the exported textures or create a sequence
        # For now, we'll just set up the file node for sequence playback
   
        # Enable sequence mode on the file node
        cmds.setAttr(file_node + '.useFrameExtension', 1)
  
        # Calculate frame offset based on start time and rate
        # NIF uses seconds, Maya uses frames (typically 24fps)
        fps = 24.0  # Adjust based on your scene settings
        
 if rate > 0:
   frames_per_texture = fps / rate
        else:
     frames_per_texture = fps  # Default to 1 second per frame
 
        # Set up expression to cycle through textures
        # Maya's frame extension cycles automatically based on the number of files
        # We just need to set the frame offset
        start_frame = start_time * fps
     
 # Set frame offset
 if not cmds.attributeQuery('frameOffset', node=file_node, exists=True):
            cmds.addAttr(file_node, longName='frameOffset', attributeType='float', defaultValue=0)
     cmds.setAttr(file_node + '.frameOffset', keyable=True)
        
        cmds.setAttr(file_node + '.frameOffset', -start_frame)
   
        # Create expression to slow down or speed up the texture playback
   expression_str = "{}.frameExtension = floor((frame - {}) / {}) % {};".format(
   file_node, start_frame, frames_per_texture, num_frames
        )
        
     expr_name = material + "_textureFlipExpr"
        
   # Delete existing expression if it exists
        if cmds.objExists(expr_name):
       cmds.delete(expr_name)
  
        # Create new expression
        cmds.expression(name=expr_name, string=expression_str)
        
     print("  Success! Created expression: " + expr_name)
  flipped_count += 1
    
    if flipped_count > 0:
 print("\nSuccessfully setup {} texture flippers".format(flipped_count))
        print("\nNote: You may need to rename your exported textures to follow Maya's")
     print("frame sequence naming convention: basename.####.ext")
        print("For example: texture_0001.png, texture_0002.png, etc.")
  else:
 print("\nNo texture flippers found in the scene.")
        print("Make sure you imported an FBX exported from NIF with texture flip controllers.")

def list_texture_flippers():
    """
    Lists all materials with texture flipper properties.
    """
    materials = cmds.ls(type=['phong', 'lambert', 'blinn'])
    
    flippers = []
    for material in materials:
     if cmds.attributeQuery('NifTextureFlipFrames', node=material, exists=True):
   try:
                start_time = cmds.getAttr(material + '.NifTextureFlipStartTime')
rate = cmds.getAttr(material + '.NifTextureFlipRate')
       num_frames = cmds.getAttr(material + '.NifTextureFlipFrames')
                flippers.append({
 'material': material,
           'start_time': start_time,
         'rate': rate,
         'frames': num_frames
    })
       except:
      pass
    
    if flippers:
        print("\nFound {} materials with texture flippers:".format(len(flippers)))
        for flipper in flippers:
            print("  {}: {} frames @ {} fps, starting at {}s".format(
           flipper['material'], 
     flipper['frames'],
         flipper['rate'],
                flipper['start_time']
      ))
    else:
 print("\nNo texture flippers found.")
    
    return flippers

# Auto-run on import if desired
if __name__ == "__main__":
    list_texture_flippers()
    print("\nTo setup texture flipping, run: setup_texture_flipping()")
