/* ps5-bapm-set2.h - BAPM parameter set 2 of the base PS5, copied verbatim from PS5 Linux (ps5-linux patches/linux.patch,
 * drivers/ps5/mp1.c: struct bapm_param_set + bapm_param_set_2, which mp1_init() applies at every boot). Only the integer
 * types were renamed (u64/u32 -> uint64_t/uint32_t). Do not edit by hand. */
#ifndef PS5_BAPM_SET2_H
#define PS5_BAPM_SET2_H
struct bapm_param_set {
	uint64_t dpm_wac[21];
	uint32_t l3_cacw[80];
	uint32_t gfx_cacw[88];
	uint64_t gfx_cacw_len;
	uint32_t bapm_param[176][2];
};

static const struct bapm_param_set ps5_bapm_set2 = {
	.dpm_wac = {
		0x0000c3adb08d0000, 0x00a800008d000000, 0xa000008e000000a6,
		0x00000000c900a500, 0xc8d000d3b8daea00, 0x0000d000a3d200dc,
		0x00bfc80000c6f100, 0x00fe00000000ad00, 0x0000abb300000000,
		0x00008600000000cf, 0x00ae0095ff000000, 0x000000000000ac00,
		0x00b00000008500ad, 0x0098849600b18000, 0xa2a38bac00b00000,
		0x97007479000092ab, 0x0095a30000008f00, 0x000000b5000000a0,
		0x0000c200a80000bd, 0x0000bcc1c4b2a300, 0xa700000000000000
	},
	.l3_cacw = {
		[71] = 0x3ff80000,
	},
	.gfx_cacw = {
		[2] = 0xefa,
		[6] = 0x7bf,
		[12] = 0x14d10000,
		[14] = 0x287b0000,
		[20] = 0x2f2,
		[22] = 0x1085ffff, 0xcd80e51, 0x4b86145a,
		[27] = 0x15a7,
		[29] = 0x13c2, 0x2d02,
		[34] = 0x78ad0000, 0x2ea0,
		[54] = 0x257d, 0x8c8,
		[58] = 0x799e,
		[63] = 0x152,
	},
	.gfx_cacw_len = 64,
	.bapm_param = {
		{1, 0xbc22f028}, {1, 0x3e113314}, {1, 0xbe8d0750}, {1, 0x3e16ae78},
		{1, 0xbe4d4746}, {1, 0x3f34470b}, {1, 0x3704564c}, {1, 0x3f516a34},
		{1, 0x3f3fdd1c}, {1, 0x3f74f5de}, {1, 0x3dec9763}, {1, 0xbc2c0ab6},
		{1, 0x3d1582c2}, {1, 0x367468e6}, {1, 0x3f059935}, {1, 0x3ef4cd95},
		{1, 0x3f74f5de}, {1, 0x3d524dae}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2},
		{1, 0x357db71b}, {1, 0x40b1089a}, {1, 0x41751b15}, {1, 0x4287d1bc},
		{1, 0x3fb89949}, {1, 0x411eefdf}, {1, 0xbc816b59}, {1, 0x3d41513a},
		[49] =
		{1, 0x41751b15}, {1, 0x4287d1bc}, {1, 0x3fb89949}, {1, 0x412d973c},
		{1, 0xbc816b59}, {1, 0x3d41513a}, {1, 0x3e0654c8}, {1, 0xbe27b465},
		{1, 0x3f2d3daa}, {1, 0x41751b15}, {1, 0x4287d1bc}, {1, 0x3fb89949},
		{1, 0x4061d4cc}, {1, 0xbc816b59}, {1, 0x3d41513a}, {1, 0x3e97c980},
		{1, 0xbf1d9856}, {1, 0x3f828e45}, {1, 0x41751b15}, {1, 0x4287d1bc},
		{1, 0x3fb89949}, {1, 0x409c4b3c}, {1, 0xbc816b59}, {1, 0x3d41513a},
		{1, 0x3e7a6107}, {1, 0xbeedd766}, {1, 0x3f67aa70}, {1, 0x41751b15},
		{1, 0x4287d1bc}, {1, 0x3fb89949}, {1, 0x40ccf558}, {1, 0xbc816b59},
		{1, 0x3d41513a}, {1, 0x3e600a8a}, {1, 0xbecbe79c}, {1, 0x3f5b627c},
		{1, 0x41751b15}, {1, 0x4287d1bc}, {1, 0x3fb89949}, {1, 0x41050555},
		{1, 0xbc816b59}, {1, 0x3d41513a}, {1, 0x3e379205}, {1, 0xbe94367e},
		{1, 0x3f46233f}, {1, 0x3f96c5af}, {1, 0x3eb99add}, {1, 0x3f85bb54},
		{1, 0x3e6a30f6}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2}, {1, 0xbbda394d},
		{1, 0x3dff4eb1}, {1, 0xbe8b186c}, {1, 0x3f8fbf3f}, {1, 0x3ee87500},
		{1, 0x3f7c5aff}, {1, 0x3e92470e}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2},
		{1, 0x396640ab}, {1, 0x3dad4b04}, {1, 0xbe6283c5}, {1, 0x3f8051ba},
		{1, 0x3f13f766}, {1, 0x3f87a42b}, {1, 0x3e52acf5}, {1, 0xbc2c0ab6},
		{1, 0x3d1582c2}, {1, 0xbbd8e837}, {1, 0x3e005ec6}, {1, 0xbe8c6197},
		{1, 0x3f61d86e}, {1, 0x3f276f79}, {1, 0x3f7b0bf0}, {1, 0x3e25ca59},
		{1, 0xbc2c0ab6}, {1, 0x3d1582c2}, {1, 0xbbc01a0e}, {1, 0x3dfb13b3},
		{1, 0xbe84dd83}, {1, 0x3fa32142}, {1, 0x3ea341fd}, {1, 0x3f85d724},
		{1, 0x3e8c3c1d}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2}, {1, 0xbaa01624},
		{1, 0x3dbebfaf}, {1, 0xbe6f57f4}, {1, 0x3f43c147}, {1, 0x3e70fae5},
		{1, 0x3f85bb54}, {1, 0x3dd02b85}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2},
		{1, 0x3f364cb6}, {1, 0x3e936695}, {1, 0x3f7c5aff}, {1, 0x3e020645},
		{1, 0xbc2c0ab6}, {1, 0x3d1582c2}, {1, 0x3f226119}, {1, 0x3ebb3dcd},
		{1, 0x3f87a42b}, {1, 0x3dbb4468}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2},
		{1, 0x3f1302b3}, {1, 0x3ed9fa9b}, {1, 0x3f7b0bf0}, {1, 0x3d935e88},
		{1, 0xbc2c0ab6}, {1, 0x3d1582c2}, {1, 0x3f4cc495}, {1, 0x3e4cedab},
		{1, 0x3f85d724}, {1, 0x3df94e6d}, {1, 0xbc2c0ab6}, {1, 0x3d1582c2},
		{1, 0x3c437be5}, {1, 0x3f3de47f}, {1, 0x3f40c129}, {1, 0x3f3cc9e7},
		{1, 0x3f41350f}, {1, 0x3f3dcec6}, {1, 0x3f3e1c51}
	}
};
#endif
