"""
将图片转换为 LVGL v8 兼容的 C 数组文件 (RGB565, byte-swapped)
用法: python convert_image.py logo.png [宽度] [高度]
默认输出 boot_logo.c，图片缩放到 120x120
"""
from PIL import Image
import sys
import os

def convert_image(input_path, output_path="boot_logo.c", width=120, height=120):
    img = Image.open(input_path)
    img = img.convert('RGB')
    img = img.resize((width, height), Image.LANCZOS)
    pixels = list(img.getdata())

    with open(output_path, 'w', encoding='utf-8') as f:
        f.write('#include "lvgl.h"\n\n')
        f.write(f'// Auto-generated from {os.path.basename(input_path)}\n')
        f.write(f'// Size: {width}x{height}, Format: RGB565 (LV_COLOR_16_SWAP=1)\n\n')
        f.write('#ifndef LV_ATTRIBUTE_MEM_ALIGN\n')
        f.write('#define LV_ATTRIBUTE_MEM_ALIGN\n')
        f.write('#endif\n\n')
        f.write('const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t boot_logo_map[] = {\n')

        for i, (r, g, b) in enumerate(pixels):
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F
            rgb565 = (r5 << 11) | (g6 << 5) | b5
            # LV_COLOR_16_SWAP = 1: 字节交换
            low = rgb565 & 0xFF
            high = (rgb565 >> 8) & 0xFF
            f.write(f'  0x{low:02x}, 0x{high:02x},')
            if (i + 1) % 12 == 0:
                f.write('\n')

        f.write('\n};\n\n')
        f.write(f'const lv_img_dsc_t boot_logo = {{\n')
        f.write(f'  .header.cf = LV_IMG_CF_TRUE_COLOR,\n')
        f.write(f'  .header.always_zero = 0,\n')
        f.write(f'  .header.reserved = 0,\n')
        f.write(f'  .header.w = {width},\n')
        f.write(f'  .header.h = {height},\n')
        f.write(f'  .data_size = {width * height * 2},\n')
        f.write(f'  .data = boot_logo_map,\n')
        f.write(f'}};\n')

    print(f'OK: {input_path} -> {output_path} ({width}x{height}, {width*height*2} bytes)')

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("用法: python convert_image.py <图片路径> [宽度] [高度]")
        print("示例: python convert_image.py logo.png 120 120")
        sys.exit(1)
    input_path = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    convert_image(input_path, "boot_logo.c", w, h)
