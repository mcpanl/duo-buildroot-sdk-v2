import sys

WIDTH = 240
HEIGHT = 320
ASCII_CHARS = "@%#*+=-:. "  # 从黑到白的字符映射（可以调整）

def rgb565_to_gray(high_byte, low_byte):
    value = (high_byte << 8) | low_byte
    r = ((value >> 11) & 0x1F) << 3
    g = ((value >> 5) & 0x3F) << 2
    b = (value & 0x1F) << 3
    gray = int(0.299*r + 0.587*g + 0.114*b)
    return gray

def gray_to_char(gray):
    index = int(gray / 256 * len(ASCII_CHARS))
    return ASCII_CHARS[min(index, len(ASCII_CHARS) - 1)]

def print_image(filename):
    with open(filename, 'rb') as f:
        for y in range(HEIGHT):
            line = ''
            for x in range(WIDTH):
                hb = f.read(1)
                lb = f.read(1)
                if not lb:
                    break
                gray = rgb565_to_gray(hb[0], lb[0])
                char = gray_to_char(gray)
                line += char
            print(line)

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("用法: python3 rgb565_viewer.py image.rgb565")
    else:
        print_image(sys.argv[1])

