key = input("Geben Sie Ihren 16-stelligen Schlüssel ein: ")
hex_values = [f"0x{ord(c):02X}" for c in key]
print("AES_KEY für den Arduino-Code:")
print(",".join(hex_values))
