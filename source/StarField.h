// StarField.h

#ifndef STAR_FIELD_H_
#define STAR_FIELD_H_

#include "Shader.h"

#include "gl_header.h"

#include <vector>

class Body;
class Point;
class Sprite;



// Object to hold a set of "stars" to be drawn as a backdrop. The star pattern
// repeats every 4096 pixels. The pattern is generated by a random walk method
// so that some parts will be much denser than others, which is visually more
// interesting than if the stars were evenly spread out in perfectly random
// noise. If the view is moving, the stars are elongated in a motion blur to
// match the motion; otherwise they would seem to jitter around.
class StarField {
public:
	void Init(int stars, int width);
	void SetHaze(const Sprite *sprite);
	
	void Draw(const Point &pos, const Point &vel, double zoom = 1.) const;
	
	
private:
	void SetUpGraphics();
	void MakeStars(int stars, int width);
	
	
private:
	int widthMod;
	int tileCols;
	std::vector<int> tileIndex;
	
	std::vector<Body> haze;
	
	Shader shader;
	GLuint vao;
	GLuint vbo;
	
	GLuint offsetI;
	GLuint sizeI;
	GLuint cornerI;
	
	GLuint scaleI;
	GLuint rotateI;
	GLuint elongationI;
	GLuint translateI;
	GLuint brightnessI;
};



#endif
