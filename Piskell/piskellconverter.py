pixellist = [
    #PISKELL exported C data here
    ]
num=0
output = ""

for point in pixellist:
    output += str(int((point-0xff000000)*500/0xffffff)+500)+", "
    num +=1
    if num == 27:
        num = 0
        output += "\n"
print(output)
