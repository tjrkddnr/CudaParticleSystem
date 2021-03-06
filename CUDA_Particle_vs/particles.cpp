/*
 * Copyright 1993-2015 NVIDIA Corporation.  All rights reserved.
 *
 * Please refer to the NVIDIA end user license agreement (EULA) associated
 * with this source code for terms and conditions that govern your use of
 * this software. Any use, reproduction, disclosure, or distribution of
 * this software and related documentation outside the terms of the EULA
 * is strictly prohibited.
 *
 */

/*
    Particle system example with collisions using uniform grid

    CUDA 2.1 SDK release 12/2008
    - removed atomic grid method, some optimization, added demo mode.

    CUDA 2.2 release 3/2009
    - replaced sort function with latest radix sort, now disables v-sync.
    - added support for automated testing and comparison to a reference value.
*/

// OpenGL Graphics includes
#include "helper_gl.h"
#if defined (WIN32)
#include "gl_helper.h"
#endif


// CUDA runtime
#include <cuda_runtime.h>

// CUDA utilities and system includes
#include "helper_functions.h"
#include "helper_cuda.h"    // includes cuda.h and cuda_runtime_api.h

// Includes
#include <stdlib.h>
#include <cstdlib>
#include <cstdio>
#include <algorithm>

#include "particleSystem.h"
#include "render_particles.h"
#include "paramgl.h"

//#include "rayTracing.cuh"
#define MAX_EPSILON_ERROR 5.00f
#define THRESHOLD         0.30f

#define GRID_SIZE       64
#define NUM_PARTICLES   16384

const uint width = 640, height = 480;

bool skybox = false;

// view params
int ox, oy;
int buttonState = 0;
float camera_trans[] = {0, 0, -3};
float camera_rot[]   = {0, 0, 0};
float camera_trans_lag[] = {0, 0, -3};
float camera_rot_lag[] = {0, 0, 0};
const float inertia = 0.1f;
ParticleRenderer::DisplayMode displayMode = ParticleRenderer::PARTICLE_SPHERES;

bool raytracer = false;
float radius;

int mode = 0;
bool displayEnabled = true;
bool bPause = false;
bool displaySliders = false;
bool wireframe = false;
bool demoMode = false;
int idleCounter = 0;
int demoCounter = 0;
const int idleDelay = 2000;

enum { M_VIEW = 0, M_MOVE };

uint numParticles = 0;
uint3 gridSize;
int numIterations = 0; // run until exit

// simulation parameters
float timestep = 0.5f;
float damping = 1.0f;
float gravity = 0.0003f;
int iterations = 1;
int ballr = 10;

float collideSpring = 0.5f;;
float collideDamping = 0.02f;;
float collideShear = 0.1f;
float collideAttraction = 0.0f;

ParticleSystem *psystem = 0;

// fps
static int fpsCount = 0;
static int fpsLimit = 1;
StopWatchInterface *timer = NULL;

ParticleRenderer *renderer = 0;

float modelView[16];
float projection[16];
int viewPort[4];

ParamListGL *params;

// Auto-Verification Code
const int frameCheckNumber = 4;
unsigned int frameCount = 0;
unsigned int g_TotalErrors = 0;
char        *g_refFile = NULL;

const char *sSDKsample = "CUDA Particles Simulation";

extern "C" void cudaInit(int argc, char **argv);
extern "C" void cudaGLInit(int argc, char **argv);
extern "C" void copyArrayFromDevice(void *host, const void *device, unsigned int vbo, int size);

float bunny[2503];

//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////
//RAY TRACING
/*
cudaGraphicsResource *resource;

GLuint pbo;
GLuint rayTracingTex;

void initRayTracer()
{
	// set pbo
	glGenBuffersARB(1, &rayTracingTex);
	glBindBufferARB(GL_PIXEL_UNPACK_BUFFER_ARB, rayTracingTex);
	glBufferDataARB(GL_PIXEL_UNPACK_BUFFER_ARB, width * height * 4, NULL, GL_DYNAMIC_DRAW_ARB);

	// set device handle
	checkCudaErrors(cudaGraphicsGLRegisterBuffer(&resource, rayTracingTex, cudaGraphicsMapFlagsNone));
}
*/



//////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////////


// initialize particle system
void initParticleSystem(int numParticles, uint3 gridSize, bool bUseOpenGL)
{
    psystem = new ParticleSystem(numParticles, gridSize, bUseOpenGL);
    psystem->reset(ParticleSystem::CONFIG_GRID);

    if (bUseOpenGL)
    {
        renderer = new ParticleRenderer;
        renderer->setParticleRadius(psystem->getParticleRadius());
        renderer->setColorBuffer(psystem->getColorBuffer());
		radius = psystem->getParticleRadius();
    }

    sdkCreateTimer(&timer);
}

void cleanup()
{
    sdkDeleteTimer(&timer);

    if (psystem)
    {
        delete psystem;
    }
    return;
}

// initialize OpenGL
void initGL(int *argc, char **argv)
{
    glutInit(argc, argv);
    glutInitDisplayMode(GLUT_RGB | GLUT_DEPTH | GLUT_DOUBLE);
    glutInitWindowSize(width, height);
    glutCreateWindow("CUDA Particles");

    if (!isGLVersionSupported(2,0) ||
        !areGLExtensionsSupported("GL_ARB_multitexture GL_ARB_vertex_buffer_object"))
    {
        fprintf(stderr, "Required OpenGL extensions missing.");
        exit(EXIT_FAILURE);
    }
	/*
#if defined (WIN32)

    if (wglewIsSupported("WGL_EXT_swap_control"))
    {
        // disable vertical sync
        wglSwapIntervalEXT(0);
    }

#endif
	*/

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.25, 0.25, 0.25, 1.0);

    glutReportErrors();
}

void runBenchmark(int iterations, char *exec_path)
{
    printf("Run %u particles simulation for %d iterations...\n\n", numParticles, iterations);
    cudaDeviceSynchronize();
    sdkStartTimer(&timer);

    for (int i = 0; i < iterations; ++i)
    {
        psystem->update(timestep);
    }

    cudaDeviceSynchronize();
    sdkStopTimer(&timer);
    float fAvgSeconds = ((float)1.0e-3 * (float)sdkGetTimerValue(&timer)/(float)iterations);

    printf("particles, Throughput = %.4f KParticles/s, Time = %.5f s, Size = %u particles, NumDevsUsed = %u, Workgroup = %u\n",
           (1.0e-3 * numParticles)/fAvgSeconds, fAvgSeconds, numParticles, 1, 0);

    if (g_refFile)
    {
        printf("\nChecking result...\n\n");
        float *hPos = (float *)malloc(sizeof(float)*4*psystem->getNumParticles());
        copyArrayFromDevice(hPos, psystem->getCudaPosVBO(),
                            0, sizeof(float)*4*psystem->getNumParticles());

        sdkDumpBin((void *)hPos, sizeof(float)*4*psystem->getNumParticles(), "particles.bin");

        if (!sdkCompareBin2BinFloat("particles.bin", g_refFile, 4*psystem->getNumParticles(),
                                    MAX_EPSILON_ERROR, THRESHOLD, exec_path))
        {
            g_TotalErrors++;
        }
    }
}

float start = 0;
float end = 0;
float delta1 = 0;
float delta2 = 0;

void computeFPS()
{
    frameCount++;
    fpsCount++;

    if (fpsCount == fpsLimit)
    {
        char fps[256];
        float ifps = 1.f / (sdkGetAverageTimerValue(&timer) / 1000.f);
        sprintf(fps, "(%d particles: %3.1f fps) (Sim time : %3.0f ms) (Rendering time : %3.0f ms)", numParticles, ifps, delta1, delta2);

        glutSetWindowTitle(fps);
        fpsCount = 0;

        fpsLimit = (int)MAX(ifps, 1.f);
        sdkResetTimer(&timer);
    }
}


void display()
{
    sdkStartTimer(&timer);

    // update the simulation
    if (!bPause)
    {
        psystem->setIterations(iterations);
        psystem->setDamping(damping);
        psystem->setGravity(-gravity);
        psystem->setCollideSpring(collideSpring);
        psystem->setCollideDamping(collideDamping);
        psystem->setCollideShear(collideShear);
        psystem->setCollideAttraction(collideAttraction);

		////////////////////////////////////////////////////////////////////
		/* simulation */
		start = clock();
        psystem->update(timestep);
		end = clock();
		delta1= end - start;
		////////////////////////////////////////////////////////////////////

        if (renderer)
        {
            renderer->setVertexBuffer(psystem->getCurrentReadBuffer(), psystem->getNumParticles());
        }
    }

    // render
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // view transform
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    for (int c = 0; c < 3; ++c)
    {
        camera_trans_lag[c] += (camera_trans[c] - camera_trans_lag[c]) * inertia;
        camera_rot_lag[c] += (camera_rot[c] - camera_rot_lag[c]) * inertia;
    }

    glTranslatef(camera_trans_lag[0], camera_trans_lag[1], camera_trans_lag[2]);
    glRotatef(camera_rot_lag[0], 1.0, 0.0, 0.0);
    glRotatef(camera_rot_lag[1], 0.0, 1.0, 0.0);

	
	float model[16];
	float view[16];

	glGetFloatv(GL_MODELVIEW_MATRIX, modelView);
	glGetFloatv(GL_PROJECTION_MATRIX, projection);

	memcpy(model, modelView, sizeof(modelView));
	model[0] = model[1] = model[2] = model[4] = model[5] = model[6] = model[8] = model[9] = model[10] = 0.0f; // remove view matrix from the modelview matrix (remove bottom right parts)

	memcpy(view, modelView, sizeof(modelView));
	view[3] = view[7] = view[11] = view[12] = view[13] = view[14] = view[15] = 0.0f; // remove model matrix from the modelview matrix (take upper left 3x3 matrix)

	renderer->displaySkyBox(view, projection);

    // cube
    glColor3f(1.0, 1.0, 1.0);
    glutWireCube(2.0);

    // collider
    glPushMatrix();
    float3 p = psystem->getColliderPos();
    glTranslatef(p.x, p.y, p.z);
    glColor3f(1.0, 0.0, 0.0);
    glutSolidSphere(psystem->getColliderRadius(), 20, 10);
    glPopMatrix();


	////////////////////////////////////////////////////////////////////
	/* Rendering */
	start = clock();
    if (renderer && displayEnabled)
    {
		skybox ? renderer->displaySpheres(model) : renderer->display(displayMode);
    }
	end = clock();
	delta2 = end - start;
	////////////////////////////////////////////////////////////////////


    if (displaySliders)
    {
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO); // invert color
        glEnable(GL_BLEND);
        params->Render(0, 0);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
	
	

    sdkStopTimer(&timer);

    glutSwapBuffers();
    glutReportErrors();

    computeFPS();
}
/*
void displayRayTracing()
{
	sdkStartTimer(&timer);

	float *_pos;
	float *pos;

	// update the simulation
	if (!bPause)
	{
		psystem->setIterations(iterations);
		psystem->setDamping(damping);
		psystem->setGravity(-gravity);
		psystem->setCollideSpring(collideSpring);
		psystem->setCollideDamping(collideDamping);
		psystem->setCollideShear(collideShear);
		psystem->setCollideAttraction(collideAttraction);

		psystem->update(timestep);

		//read the buffer
		glBindBuffer(GL_ARRAY_BUFFER, psystem->getCurrentReadBuffer());
		_pos = (float*)glMapBuffer(GL_ARRAY_BUFFER, GL_READ_ONLY);
		pos = (float*)malloc(4 * sizeof(float) *  psystem->getNumParticles());
		memcpy(pos, _pos, sizeof(pos));
		glUnmapBuffer(GL_ARRAY_BUFFER);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		//now we have particle's xyzh data in pos array 
	}

	// render
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// view transform
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	for (int c = 0; c < 3; ++c)
	{
		camera_trans_lag[c] += (camera_trans[c] - camera_trans_lag[c]) * inertia;
		camera_rot_lag[c] += (camera_rot[c] - camera_rot_lag[c]) * inertia;
	}

	




	//1) create objects


	//2) set lookAt
	glTranslatef(camera_trans_lag[0], camera_trans_lag[1], camera_trans_lag[2]);
	glRotatef(camera_rot_lag[0], 1.0, 0.0, 0.0);
	glRotatef(camera_rot_lag[1], 0.0, 1.0, 0.0);
	


	//3) ray tracing process


	//4) draw


	glGetFloatv(GL_MODELVIEW_MATRIX, modelView);
	glGetFloatv(GL_PROJECTION_MATRIX, projection);
	glGetIntegerv(GL_VIEWPORT, viewPort);



	
	//reset the lookat
	glTranslatef(-camera_trans_lag[0], -camera_trans_lag[1], -camera_trans_lag[2]);
	glRotatef(-camera_rot_lag[0], 1.0, 0.0, 0.0);
	glRotatef(-camera_rot_lag[1], 0.0, 1.0, 0.0);

	// map device memory with resource
	uchar4 *devPtr;
	size_t size;
	checkCudaErrors(cudaGraphicsMapResources(1, &resource, NULL));

	lunchRayTracer(devPtr, modelView, projection, viewPort);

	checkCudaErrors(cudaGraphicsResourceGetMappedPointer((void**)&devPtr, &size, resource));


	glDrawPixels(width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);



	if (displaySliders)
	{
		glDisable(GL_DEPTH_TEST);
		glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ZERO); // invert color
		glEnable(GL_BLEND);
		params->Render(0, 0);
		glDisable(GL_BLEND);
		glEnable(GL_DEPTH_TEST);
	}

	sdkStopTimer(&timer);

	glutSwapBuffers();
	glutReportErrors();

	computeFPS();
}

*/
inline float frand()
{
    return rand() / (float) RAND_MAX;
}

void addSphere()
{
    // inject a sphere of particles
    float pr = psystem->getParticleRadius();
    float tr = pr+(pr*2.0f)*ballr;
    float pos[4], vel[4];
    pos[0] = -1.0f + tr + frand()*(2.0f - tr*2.0f);
    pos[1] = 1.0f - tr;
    pos[2] = -1.0f + tr + frand()*(2.0f - tr*2.0f);
    pos[3] = 0.0f;
    vel[0] = vel[1] = vel[2] = vel[3] = 0.0f;
    psystem->addSphere(0, pos, vel, ballr, pr*2.0f);
}

void reshape(int w, int h)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, (float) w / (float) h, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glViewport(0, 0, w, h);

    if (renderer)
    {
        renderer->setWindowSize(w, h);
        renderer->setFOV(60.0);
    }

	//glGetFloatv(GL_PROJECTION_MATRIX, projection);
}

void mouse(int button, int state, int x, int y)
{
    int mods;

    if (state == GLUT_DOWN)
    {
        buttonState |= 1<<button;
    }
    else if (state == GLUT_UP)
    {
        buttonState = 0;
    }

    mods = glutGetModifiers();

    if (mods & GLUT_ACTIVE_SHIFT)
    {
        buttonState = 2;
    }
    else if (mods & GLUT_ACTIVE_CTRL)
    {
        buttonState = 3;
    }

    ox = x;
    oy = y;

    demoMode = false;
    idleCounter = 0;

    if (displaySliders)
    {
        if (params->Mouse(x, y, button, state))
        {
            glutPostRedisplay();
            return;
        }
    }

    glutPostRedisplay();
}

// transform vector by matrix
void xform(float *v, float *r, GLfloat *m)
{
    r[0] = v[0]*m[0] + v[1]*m[4] + v[2]*m[8] + m[12];
    r[1] = v[0]*m[1] + v[1]*m[5] + v[2]*m[9] + m[13];
    r[2] = v[0]*m[2] + v[1]*m[6] + v[2]*m[10] + m[14];
}

// transform vector by transpose of matrix
void ixform(float *v, float *r, GLfloat *m)
{
    r[0] = v[0]*m[0] + v[1]*m[1] + v[2]*m[2];
    r[1] = v[0]*m[4] + v[1]*m[5] + v[2]*m[6];
    r[2] = v[0]*m[8] + v[1]*m[9] + v[2]*m[10];
}

void ixformPoint(float *v, float *r, GLfloat *m)
{
    float x[4];
    x[0] = v[0] - m[12];
    x[1] = v[1] - m[13];
    x[2] = v[2] - m[14];
    x[3] = 1.0f;
    ixform(x, r, m);
}

void motion(int x, int y)
{
    float dx, dy;
    dx = (float)(x - ox);
    dy = (float)(y - oy);

    if (displaySliders)
    {
        if (params->Motion(x, y))
        {
            ox = x;
            oy = y;
            glutPostRedisplay();
            return;
        }
    }

    switch (mode)
    {
        case M_VIEW:
            if (buttonState == 3)
            {
                // left+middle = zoom
                camera_trans[2] += (dy / 100.0f) * 0.5f * fabs(camera_trans[2]);
            }
            else if (buttonState & 2)
            {
                // middle = translate
                camera_trans[0] += dx / 100.0f;
                camera_trans[1] -= dy / 100.0f;
            }
            else if (buttonState & 1)
            {
                // left = rotate
                camera_rot[0] += dy / 5.0f;
                camera_rot[1] += dx / 5.0f;
            }

            break;

        case M_MOVE:
            {
                float translateSpeed = 0.003f;
                float3 p = psystem->getColliderPos();

                if (buttonState==1)
                {
                    float v[3], r[3];
                    v[0] = dx*translateSpeed;
                    v[1] = -dy*translateSpeed;
                    v[2] = 0.0f;
                    ixform(v, r, modelView);
                    p.x += r[0];
                    p.y += r[1];
                    p.z += r[2];
                }
                else if (buttonState==2)
                {
                    float v[3], r[3];
                    v[0] = 0.0f;
                    v[1] = 0.0f;
                    v[2] = dy*translateSpeed;
                    ixform(v, r, modelView);
                    p.x += r[0];
                    p.y += r[1];
                    p.z += r[2];
                }

                psystem->setColliderPos(p);
            }
            break;
    }

    ox = x;
    oy = y;

    demoMode = false;
    idleCounter = 0;

    glutPostRedisplay();
}

// commented out to remove unused parameter warnings in Linux
void key(unsigned char key, int /*x*/, int /*y*/)
{
    switch (key)
    {
        case ' ':
            bPause = !bPause;
            break;

        case 13:
            psystem->update(timestep);

            if (renderer)
            {
                renderer->setVertexBuffer(psystem->getCurrentReadBuffer(), psystem->getNumParticles());
            }

            break;

        case '\033':
        case 'q':
            #if defined(__APPLE__) || defined(MACOSX)
                exit(EXIT_SUCCESS);
            #else
                glutDestroyWindow(glutGetWindow());
                return;
            #endif
        case 'v':
            mode = M_VIEW;
            break;

        case 'm':
            mode = M_MOVE;
            break;

        case 'p':
            displayMode = (ParticleRenderer::DisplayMode)
                          ((displayMode + 1) % ParticleRenderer::PARTICLE_NUM_MODES);
            break;

        case 'd':
            psystem->dumpGrid();
            break;

        case 'u':
            psystem->dumpParticles(0, numParticles-1);
            break;

        case 'r':
            displayEnabled = !displayEnabled;
            break;

        case '1':
            psystem->reset(ParticleSystem::CONFIG_GRID);
            break;

        case '2':
            psystem->reset(ParticleSystem::CONFIG_RANDOM);
            break;

        case '3':
            addSphere();
            break;

        case '4':
            {
                // shoot ball from camera
                float pr = psystem->getParticleRadius();
                float vel[4], velw[4], pos[4], posw[4];
                vel[0] = 0.0f;
                vel[1] = 0.0f;
                vel[2] = -0.05f;
                vel[3] = 0.0f;
                ixform(vel, velw, modelView);

                pos[0] = 0.0f;
                pos[1] = 0.0f;
                pos[2] = -2.5f;
                pos[3] = 1.0;
                ixformPoint(pos, posw, modelView);
                posw[3] = 0.0f;

                psystem->addSphere(0, posw, velw, ballr, pr*2.0f);
            }
            break;

        case 'w':
            wireframe = !wireframe;
            break;

        case 'h':
            displaySliders = !displaySliders;
            break;
    }

    demoMode = false;
    idleCounter = 0;
    glutPostRedisplay();
}

void special(int k, int x, int y)
{
    if (displaySliders)
    {
        params->Special(k, x, y);
    }

    demoMode = false;
    idleCounter = 0;
}

void idle(void)
{
    if ((idleCounter++ > idleDelay) && (demoMode==false))
    {
        demoMode = true;
        printf("Entering demo mode\n");
    }

    if (demoMode)
    {
        camera_rot[1] += 0.1f;

        if (demoCounter++ > 1000)
        {
            ballr = 10 + (rand() % 10);
            addSphere();
            demoCounter = 0;
        }
    }

    glutPostRedisplay();
}

void initParams()
{
    if (g_refFile)
    {
        timestep = 0.0f;
        damping = 0.0f;
        gravity = 0.0f;
        ballr = 1;
        collideSpring = 0.0f;
        collideDamping = 0.0f;
        collideShear = 0.0f;
        collideAttraction = 0.0f;
    }
    else
    {
        // create a new parameter list
        params = new ParamListGL("misc");
        params->AddParam(new Param<float>("time step", timestep, 0.0f, 1.0f, 0.01f, &timestep));
        params->AddParam(new Param<float>("damping"  , damping , 0.0f, 1.0f, 0.001f, &damping));
        params->AddParam(new Param<float>("gravity"  , gravity , 0.0f, 0.001f, 0.0001f, &gravity));
        params->AddParam(new Param<int> ("ball radius", ballr , 1, 20, 1, &ballr));

        params->AddParam(new Param<float>("collide spring" , collideSpring , 0.0f, 1.0f, 0.001f, &collideSpring));
        params->AddParam(new Param<float>("collide damping", collideDamping, 0.0f, 0.1f, 0.001f, &collideDamping));
        params->AddParam(new Param<float>("collide shear"  , collideShear  , 0.0f, 0.1f, 0.001f, &collideShear));
        params->AddParam(new Param<float>("collide attract", collideAttraction, 0.0f, 0.1f, 0.001f, &collideAttraction));
    }
}

void mainMenu(int i)
{
    key((unsigned char) i, 0, 0);
}

void initMenus()
{
    glutCreateMenu(mainMenu);
    glutAddMenuEntry("Reset block [1]", '1');
    glutAddMenuEntry("Reset random [2]", '2');
    glutAddMenuEntry("Add sphere [3]", '3');
    glutAddMenuEntry("View mode [v]", 'v');
    glutAddMenuEntry("Move cursor mode [m]", 'm');
    glutAddMenuEntry("Toggle point rendering [p]", 'p');
    glutAddMenuEntry("Toggle animation [ ]", ' ');
    glutAddMenuEntry("Step animation [ret]", 13);
    glutAddMenuEntry("Toggle sliders [h]", 'h');
    glutAddMenuEntry("Quit (esc)", '\033');
    glutAttachMenu(GLUT_RIGHT_BUTTON);
}

////////////////////////////////////////////////////////////////////////////////
// Program main
////////////////////////////////////////////////////////////////////////////////
int
main(int argc, char **argv)
{
	int scene;
	scanf("%d", &scene);
	if (scene == 1) skybox = false;
	else skybox = true;
#if defined(__linux__)
    setenv ("DISPLAY", ":0", 0);
#endif

    printf("%s Starting...\n\n", sSDKsample);

    printf("NOTE: The CUDA Samples are not meant for performance measurements. Results may vary when GPU Boost is enabled.\n\n");
	skybox ? numParticles = 4096 : numParticles = NUM_PARTICLES;
    
    uint gridDim = GRID_SIZE;
    numIterations = 0;

    if (argc > 1)
    {
        if (checkCmdLineFlag(argc, (const char **) argv, "n"))
        {
            numParticles = getCmdLineArgumentInt(argc, (const char **)argv, "n");
        }

        if (checkCmdLineFlag(argc, (const char **) argv, "grid"))
        {
            gridDim = getCmdLineArgumentInt(argc, (const char **) argv, "grid");
        }

        if (checkCmdLineFlag(argc, (const char **)argv, "file"))
        {
            getCmdLineArgumentString(argc, (const char **)argv, "file", &g_refFile);
            fpsLimit = frameCheckNumber;
            numIterations = 1;
        }
    }

    gridSize.x = gridSize.y = gridSize.z = gridDim;
    printf("grid: %d x %d x %d = %d cells\n", gridSize.x, gridSize.y, gridSize.z, gridSize.x*gridSize.y*gridSize.z);
    printf("particles: %d\n", numParticles);

    bool benchmark = checkCmdLineFlag(argc, (const char **) argv, "benchmark") != 0;

    if (checkCmdLineFlag(argc, (const char **) argv, "i"))
    {
        numIterations = getCmdLineArgumentInt(argc, (const char **) argv, "i");
    }

    if (benchmark || g_refFile)
    {
        cudaInit(argc, argv);
    }
    else
    {
        if (checkCmdLineFlag(argc, (const char **)argv, "device"))
        {
            printf("[%s]\n", argv[0]);
            printf("   Does not explicitly support -device=n in OpenGL mode\n");
            printf("   To use -device=n, the sample must be running w/o OpenGL\n\n");
            printf(" > %s -device=n -file=<*.bin>\n", argv[0]);
            printf("exiting...\n");
            exit(EXIT_SUCCESS);
        }

        initGL(&argc, argv);
        cudaInit(argc, argv);
    }

    initParticleSystem(numParticles, gridSize, !benchmark && g_refFile==NULL);
    initParams();
	//initRayTracer();
    if (benchmark || g_refFile)
    {
        if (numIterations <= 0)
        {
            numIterations = 300;
        }

        runBenchmark(numIterations, argv[0]);
    }
    else
    {
        if (!g_refFile)
        {
            initMenus();
        }

        glutDisplayFunc(display);
        glutReshapeFunc(reshape);
        glutMouseFunc(mouse);
        glutMotionFunc(motion);
        glutKeyboardFunc(key);
        glutSpecialFunc(special);
        glutIdleFunc(idle);

        //glutCloseFunc(cleanup);

        glutMainLoop();
    }

    if (psystem)
    {
        delete psystem;
    }

    exit(g_TotalErrors > 0 ? EXIT_FAILURE : EXIT_SUCCESS);
}

