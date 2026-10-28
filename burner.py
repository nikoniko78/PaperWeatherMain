import os
import glob

def generate_images_header():
    img_dir = "images/Default [Pixel]"
    os.makedirs("include", exist_ok=True)
    pngs = glob.glob(os.path.join(img_dir, "*.png"))
    
    code = "#pragma once\n#include <Arduino.h>\n\ninline const uint8_t* getEmbeddedPng(const char* path, size_t& len) {\n"
    for p in pngs:
        base = os.path.splitext(os.path.basename(p))[0]
        name = "img_" + base.replace("-", "_").replace(" ", "_").replace("[", "").replace("]", "")
        with open(p, "rb") as f: 
            data = f.read()
        hex_data = ", ".join(f"0x{b:02x}" for b in data)
        code += f"    static const uint8_t {name}[] = {{{hex_data}}};\n"
    
    code += "    len = 0;\n"
    for p in pngs:
        base = os.path.splitext(os.path.basename(p))[0]
        name = "img_" + base.replace("-", "_").replace(" ", "_").replace("[", "").replace("]", "")
        fname = "/" + os.path.basename(p)
        code += f"    if (strcmp(path, \"{fname}\") == 0) {{ len = sizeof({name}); return {name}; }}\n"
        
    code += "    return nullptr;\n}\n"
    
    with open("include/images.h", "w") as f:
        f.write(code)
    print(f"Burner: Successfully generated include/images.h from {len(pngs)} pixel images.")

if __name__ == "__main__":
    generate_images_header()
