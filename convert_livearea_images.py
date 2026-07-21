import os
from PIL import Image

def process_image(input_path, output_path, size):
    try:
        if not os.path.exists(input_path):
            print(f"File not found: {input_path}")
            return
            
        with Image.open(input_path) as img:
            # Convert to RGBA first to ensure alpha channel is preserved correctly
            img = img.convert('RGBA')
            
            # Resize
            img = img.resize(size, Image.Resampling.LANCZOS)
            
            # Convert to 8-bit indexed with palette (P mode)
            # quantize handles RGBA to P conversion well and produces 8-bit PNGs.
            img_quant = img.quantize(colors=256, method=Image.Quantize.FASTOCTREE)
            
            # Ensure the output directory exists
            os.makedirs(os.path.dirname(output_path), exist_ok=True)
            
            # Save the image
            img_quant.save(output_path, format='PNG', optimize=False)
            print(f"Successfully processed {os.path.basename(input_path)} -> {output_path}")
    except Exception as e:
        print(f"Error processing {input_path}: {e}")

if __name__ == "__main__":
    base_dir = "/mnt/e/PSVITA Develop/Inotia3-vita"
    
    tasks = [
        (os.path.join(base_dir, "inotia3-1-3-2/res/drawable-xhdpi/icon.png"), 
         os.path.join(base_dir, "extras/livearea/icon0.png"), 
         (128, 128)),
         
        (os.path.join(base_dir, "Gemini_Generated_Image_4miwsm4miwsm4miw.png"), 
         os.path.join(base_dir, "extras/livearea/startup.png"), 
         (280, 158)),
         
        (os.path.join(base_dir, "Gemini_Generated_Image_4miwsm4miwsm4miw.png"), 
         os.path.join(base_dir, "extras/livearea/bg0.png"), 
         (840, 500)),
         
        (os.path.join(base_dir, "Gemini_Generated_Image_7su4hy7su4hy7su4.png"), 
         os.path.join(base_dir, "extras/livearea/pic0.png"), 
         (960, 544))
    ]
    
    for in_path, out_path, size in tasks:
        process_image(in_path, out_path, size)
