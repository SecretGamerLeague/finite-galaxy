// Color.h

#ifndef COLOR_H_
#define COLOR_H_



// Class representing an RGBA color (for use by OpenGL). The specified colors
// should be in premultiplied alpha mode. For example, a white color which is
// 50% translucent would be {.5, .5, .5} with an alpha of .5. This allows you to
// also specify colors that use additive blending. For example, if the alpha is
// zero the color's components will be added to whatever is underneath them.
class Color {
public:
	// Constructor for shades of grey, opaque unless an alpha is also given.
	explicit Color(float i = 1.f, float a = 1.f);
	// Constructor for colors, opaque unless an alpha is also given.
	Color(float r, float g, float b, float a = 1.f);
	
	// Set this color to the given RGBA values.
	void Load(double r, double g, double b, double a);
	// Get the color as a float vector, suitable for use by OpenGL.
	const float *Get() const;
	
	// Get this color, but entirely opaque. That is, this is the color you would
	// get if drawing this color on top of opaque black.
	Color Opaque() const;
	// Assuming that this color is completely opaque, get the same color with
	// the given transparency.
	Color Transparent(float alpha) const;
	// Assuming that this is an opaque color, get its additive equivalent with
	// the given fraction of its full brightness.
	Color Additive(float alpha) const;
	
	
private:
	// Store the color as a float vector for easy interfacing with OpenGL.
	float color[4];
};



#endif
