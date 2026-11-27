    .def @feat.00; .scl 3; .type 0; .endef
    .globl @feat.00
    .set @feat.00, 1

    .section .rdata,"dr"

    .global _rom_b1_u27_start
    .global _rom_b1_u27_end
_rom_b1_u27_start:
    .incbin "bin/ddonpach/b1.u27"
_rom_b1_u27_end:

    .global _rom_b2_u26_start
    .global _rom_b2_u26_end
_rom_b2_u26_start:
    .incbin "bin/ddonpach/b2.u26"
_rom_b2_u26_end:

    .global _rom_u50_start
    .global _rom_u50_end
_rom_u50_start:
    .incbin "bin/ddonpach/u50.bin"
_rom_u50_end:

    .global _rom_u51_start
    .global _rom_u51_end
_rom_u51_start:
    .incbin "bin/ddonpach/u51.bin"
_rom_u51_end:

    .global _rom_u52_start
    .global _rom_u52_end
_rom_u52_start:
    .incbin "bin/ddonpach/u52.bin"
_rom_u52_end:

    .global _rom_u53_start
    .global _rom_u53_end
_rom_u53_start:
    .incbin "bin/ddonpach/u53.bin"
_rom_u53_end:

    .global _rom_u60_start
    .global _rom_u60_end
_rom_u60_start:
    .incbin "bin/ddonpach/u60.bin"
_rom_u60_end:

    .global _rom_u61_start
    .global _rom_u61_end
_rom_u61_start:
    .incbin "bin/ddonpach/u61.bin"
_rom_u61_end:

    .global _rom_u62_start
    .global _rom_u62_end
_rom_u62_start:
    .incbin "bin/ddonpach/u62.bin"
_rom_u62_end:

    .global _rom_u6_start
    .global _rom_u6_end
_rom_u6_start:
    .incbin "bin/ddonpach/u6.bin"
_rom_u6_end:

    .global _rom_u7_start
    .global _rom_u7_end
_rom_u7_start:
    .incbin "bin/ddonpach/u7.bin"
_rom_u7_end:

    .global _rom_eeprom_start
    .global _rom_eeprom_end
_rom_eeprom_start:
    .incbin "bin/ddonpach/eeprom-ddonpach.bin"
_rom_eeprom_end:
