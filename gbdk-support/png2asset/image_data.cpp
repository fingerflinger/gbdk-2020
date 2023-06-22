#include <vector>
#include <string>
#include <algorithm>
#include <cstring>
#include <set>
#include <stdio.h>
#include <fstream>
#include <cstdint>

#include "lodepng.h"
#include "global.h"
#include "mttile.h"
#include "export.h"
#include "map_attributes.h"
#include "palettes.h"
#include "source_tileset.h"

#include "cmp_int_color.h"
#include "process_arguments.h"

using namespace std;

#include "png2asset.h"
#include "image_utils.h"
#include "maps.h"
#include "metasprites.h"

int decodePNG(vector<unsigned char>& out_image, unsigned long& image_width, unsigned long& image_height, const unsigned char* in_png, size_t in_size, bool convert_to_rgba32 = true);
void loadFile(vector<unsigned char>& buffer, const std::string& filename);

int ReadImageData_KeepPaletteOrder(vector< SetPal > palettes, vector<unsigned char> buffer, lodepng::State state, PNG2AssetData* png2AssetData) {
	//Calling with keep_palette_order means
		//-The image should be png indexed (1-8 bits per pixel)
		//-For CGB: Each 4 colors define a gbc palette, the first color is the transparent one
		//-Each rectangle with dimension(tile_w, tile_h) in the image has colors from one of those palettes only
	state.info_raw.colortype = LCT_PALETTE;
	state.info_raw.bitdepth = 8;

	// * Do *NOT* turn color_convert ON here.
	// When source PNG is indexed with bit depth was less than 8 bits per pixel then
	// color_convert may mangle the packed indexed values. Instead manually unpack them.
	//
	// This is necessary since some PNG encoders will use the minimum possible number of bits.
	//   For example 2 colors in the palette -> 1bpp -> 8 pixels per byte in the decoded image.
	//     Also see below about requirement to use palette from source image
	state.decoder.color_convert = false;

	unsigned error = lodepng::decode(png2AssetData->image.data, png2AssetData->image.w, png2AssetData->image.h, state, buffer);
	// Unpack the image if needed. Also checks and errors on incompatible palette type if needed
	if(!image_indexed_ensure_8bpp(png2AssetData->image.data, png2AssetData->image.w, png2AssetData->image.h, (int)state.info_png.color.bitdepth, (int)state.info_png.color.colortype))
		return 1;
	else if(error) {
		printf("decoder error %s\n", lodepng_error_text(error));
		return 1;
	}

	// Use source image palette since lodepng conversion to indexed (LCT_PALETTE) won't create a palette
	// So: state.info_png.color.palette/size instead of state.info_raw.palette/size
	unsigned int palette_count = PaletteCountApplyMaxLimit(png2AssetData->max_palettes, state.info_png.color.palettesize / png2AssetData->image.colors_per_pal);
	png2AssetData->image.total_color_count = palette_count * png2AssetData->image.colors_per_pal;
	png2AssetData->image.palette = state.info_png.color.palette;


	if(png2AssetData->repair_indexed_pal)
		if(!image_indexed_repair_tile_palettes(png2AssetData->image, png2AssetData->use_2x2_map_attributes))
			return 1;

	// TODO: Enable dimension check
	// // Validate image dimensions
	// if (((image.w % tile_w) != 0) || ((image.h % tile_h) != 0))
	// {
	// 	printf("Error: Image size %d x %d isn't an even multiple of tile size %d x %d\n", image.w, image.h, tile_w, tile_h);
	// 	return 1;
	// }

	if(png2AssetData->use_source_tileset) {

		// Make sure these two values match when keeping palette order
		if(png2AssetData->image.total_color_count != png2AssetData->source_tileset_image.total_color_count) {

			printf("error: The number of color palette's for your source tileset (%d) and target image (%d) do not match.", (unsigned int)png2AssetData->source_tileset_image.total_color_count, (unsigned int)png2AssetData->image.total_color_count);
			return 1;
		}

		size_t size = max(png2AssetData->image.total_color_count, png2AssetData->source_tileset_image.total_color_count);

		// Make sure these two values match when keeping palette order
		if(memcmp(png2AssetData->image.palette, png2AssetData->source_tileset_image.palette, size) != 0) {

			printf("error: The palette's for your source tileset and target image do not match.");
			return 1;
		}
	}
	return 0;
}

int ReadImageData_Default(vector< SetPal > palettes, vector<unsigned char> buffer, lodepng::State state, PNG2AssetData* png2AssetData) {
	PNGImage image32;
	image32.colors_per_pal = png2AssetData->image.colors_per_pal;
	image32.tile_w = png2AssetData->image.tile_w;
	image32.tile_h = png2AssetData->image.tile_h;

	unsigned error = lodepng::decode(image32.data, image32.w, image32.h, state, buffer); //decode as 32 bit
	if(error)
	{
		printf("decoder error %s\n", lodepng_error_text(error));
		return 1;
	}

	// Validate image dimensions
	if(((image32.w % image32.tile_w) != 0) || ((image32.h % image32.tile_h) != 0))
	{
		printf("Error: Image size %d x %d isn't an even multiple of tile size %d x %d\n", image32.w, image32.h, image32.tile_w, image32.tile_h);
		return 1;
	}

	int* palettes_per_tile = BuildPalettesAndAttributes(image32, palettes, png2AssetData->use_2x2_map_attributes);

	//Create the indexed image
	png2AssetData->image.data.clear();
	png2AssetData->image.w = image32.w;
	png2AssetData->image.h = image32.h;

	unsigned int palette_count = PaletteCountApplyMaxLimit(png2AssetData->max_palettes, palettes.size());

	png2AssetData->image.total_color_count = palette_count * png2AssetData->image.colors_per_pal;
	png2AssetData->image.palette = new unsigned char[palette_count * png2AssetData->image.colors_per_pal * RGBA32_SZ]; // total color count * 4 bytes each

	// If we are using a sourcetileset and have more palettes than it defines
	if(png2AssetData->use_source_tileset && (png2AssetData->image.total_color_count > png2AssetData->source_total_color_count)) {
		printf("Found %d extra palette(s) for target tilemap.\n", (unsigned int)((png2AssetData->image.total_color_count - png2AssetData->source_total_color_count) / png2AssetData->image.colors_per_pal));
	}
	for(size_t p = 0; p < palette_count; ++p)
	{
		int* color_ptr = (int*)&png2AssetData->image.palette[p * png2AssetData->image.colors_per_pal * RGBA32_SZ];

		//TODO: if palettes[p].size() != image.colors_per_pal we should probably try to fill the gaps based on grayscale values

		for(SetPal::iterator it = palettes[p].begin(); it != palettes[p].end(); ++it, color_ptr++)
		{
			unsigned char* c = (unsigned char*)&(*it);
			*color_ptr = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
		}
	}

	for(size_t y = 0; y < image32.h; ++y)
	{
		for(size_t x = 0; x < image32.w; ++x)
		{
			unsigned char* c32ptr = &image32.data[(image32.w * y + x) * RGBA32_SZ];
			int color32 = (c32ptr[0] << 24) | (c32ptr[1] << 16) | (c32ptr[2] << 8) | c32ptr[3];
			unsigned char palette = palettes_per_tile[(y / image32.tile_h) * (image32.w / image32.tile_w) + (x / image32.tile_w)];
			unsigned char index = std::distance(palettes[palette].begin(), palettes[palette].find(color32));
			png2AssetData->image.data.push_back((palette << png2AssetData->bpp) + index);
		}
	}

	//Test: output png to see how it looks
	//Export(image, "temp.png");
	return 0;
}

int ReadImageData( vector< SetPal > palettes, PNG2AssetData* png2AssetData) {

	png2AssetData->image.colors_per_pal = 1 << png2AssetData->bpp;

	if(png2AssetData->export_as_map)
	{
		png2AssetData->image.tile_w = 8; //Force tiles_w to 8 on maps
		png2AssetData->image.tile_h = 8; //Force tiles_h to 8 on maps
		png2AssetData->sprite_mode = SPR_NONE;
	}


	//load and decode png
	vector<unsigned char> buffer;
	lodepng::load_file(buffer, png2AssetData->input_filename);
	lodepng::State state;

	int errorCode = 0;

	// Will the specified PNG have a palette provided with it?
	if(png2AssetData->keep_palette_order) {
		
		// Save the error code
		errorCode= ReadImageData_KeepPaletteOrder(palettes, buffer, state,png2AssetData);
	}
	else
	{

		// Save the error code
		errorCode= ReadImageData_Default( palettes, buffer, state,png2AssetData);
	}

	if(errorCode != 0)return errorCode;


	if(png2AssetData->sprite_w == 0) png2AssetData->sprite_w = (int)png2AssetData->image.w;
	if(png2AssetData->sprite_h == 0) png2AssetData->sprite_h = (int)png2AssetData->image.h;
	if(png2AssetData->pivot_x == 0xFFFFFF) png2AssetData->pivot_x = png2AssetData->sprite_w / 2;
	if(png2AssetData->pivot_y == 0xFFFFFF) png2AssetData->pivot_y = png2AssetData->sprite_h / 2;
	if(png2AssetData->pivot_w == 0xFFFFFF) png2AssetData->pivot_w = png2AssetData->sprite_w;
	if(png2AssetData->pivot_h == 0xFFFFFF) png2AssetData->pivot_h = png2AssetData->sprite_h;

	return 0;
}

