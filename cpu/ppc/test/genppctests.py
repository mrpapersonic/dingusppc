def gen_ppc_opcode(opc_str, imm):
    if opc_str == "ADD":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x10A << 1)
    elif opc_str == "ADD.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x10A << 1) + 1
    elif opc_str == "ADDC":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0xA << 1)
    elif opc_str == "ADDC.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0xA << 1) + 1
    elif opc_str == "ADDCO":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x20A << 1)
    elif opc_str == "ADDCO.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x20A << 1) + 1
    elif opc_str == "ADDO":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x30A << 1)
    elif opc_str == "ADDO.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x30A << 1) + 1
    elif opc_str == "ADDE":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x8A << 1)
    elif opc_str == "ADDE.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x8A << 1) + 1
    elif opc_str == "ADDEO":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x28A << 1)
    elif opc_str == "ADDEO.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (4 << 11) + (0x28A << 1) + 1
    elif opc_str == "ADDI":
        return (0x0E << 26) + (3 << 21) + (3 << 16) + (imm & 0xFFFF)
    elif opc_str == "ADDIC":
        return (0x0C << 26) + (3 << 21) + (3 << 16) + (imm & 0xFFFF)
    elif opc_str == "ADDIS":
        return (0x0F << 26) + (3 << 21) + (3 << 16) + (imm & 0xFFFF)
    elif opc_str == "ADDME":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0xEA << 1)
    elif opc_str == "ADDME.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0xEA << 1) + 1
    elif opc_str == "ADDMEO":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0x2EA << 1)
    elif opc_str == "ADDMEO.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0x2EA << 1) + 1
    elif opc_str == "ADDZE":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0xCA << 1)
    elif opc_str == "ADDZE.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0xCA << 1) + 1
    elif opc_str == "ADDZEO":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0x2CA << 1)
    elif opc_str == "ADDZEO.":
        return (0x1F << 26) + (3 << 21) + (3 << 16) + (0x2CA << 1) + 1


def find_imm(line):
    pos = 12
    while pos < len(line):
        reg_id = line[pos:pos+4]
        if reg_id.startswith("rD") or reg_id.startswith("rA") or reg_id.startswith("rB"):
            pos += 16
        elif reg_id.startswith("XER:"):
            pos += 18
        elif reg_id.startswith("CR:"):
            pos += 17
        elif reg_id.startswith("imm"):
            return int(line[pos+4:pos+14], base=16)
    return 0


with open("instruction_tests_console.txt", "r") as in_file:
    with open("ppcinttests.csv", "w") as out_file:
        lineno = 0
        for line in in_file:
            line = line.strip()
            opcode = (line[0:8]).rstrip()
            out_file.write(opcode + ",")

            imm = find_imm(line)

            out_file.write("0x{:X}".format(gen_ppc_opcode(opcode, imm)))

            pos = 12

            while pos < len(line):
                reg_id = line[pos:pos+4]
                if reg_id.startswith("rD"):
                    out_file.write(",rD=" + line[pos+3:pos+13])
                    pos += 16
                elif reg_id.startswith("rA"):
                    out_file.write(",rA=" + line[pos+3:pos+13])
                    pos += 16
                elif reg_id.startswith("rB"):
                    out_file.write(",rB=" + line[pos+3:pos+13])
                    pos += 16
                elif reg_id.startswith("XER:"):
                    out_file.write(",XER=" + line[pos+5:pos+15])
                    pos += 18
                elif reg_id.startswith("CR:"):
                    out_file.write(",CR=" + line[pos+4:pos+14])
                    pos += 17
                elif reg_id.startswith("imm"):
                    pos += 17 # ignore immediate operands
                else:
                    out_file.write("Unknown reg ID" + reg_id)
                    break

            out_file.write("\n")

            lineno += 1
            if lineno > 152:
                break
