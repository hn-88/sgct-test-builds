/*****************************************************************************************
 * SGCT                                                                                  *
 * Simple Graphics Cluster Toolkit                                                       *
 *                                                                                       *
 * Copyright (c) 2012-2020                                                               *
 * For conditions of distribution and use, see copyright notice in LICENSE.md            *
 ****************************************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <fstream>
#include <algorithm> //used for transform string to lowercase
#include <sgct/sgct.h>
#include <sgct/window.h>
#include <sgct/openvr.h>
#include <sgct/image.h>
#include <sgct/ClusterManager.h>

sgct::Engine * gEngine;
sgct::Window* FirstOpenVRWindow = NULL;

constexpr const char* vertexShader = R"(
  #version 330 core

  layout(location = 0) in vec2 texCoords;
  layout(location = 1) in vec3 normals;
  layout(location = 2) in vec3 vertPositions;

  uniform mat4 mvp;

  out vec2 uv;

  void main() {
    // Output position of the vertex, in clip space : MVP * position
    gl_Position = mvp * vec4(vertPositions, 1.0);
    uv = texCoords;
  })";

constexpr const char* fragmentShader = R"(
  #version 330 core

  uniform sampler2D tex;

  in vec2 uv;
  out vec4 color;

  void main() { color = texture(tex, uv); }
)";


void myInitOGLFun();
void myPreSyncFun();
void myPostSyncPreDrawFun();
void myDrawFun();
void myPostDrawFun();
void myEncodeFun();
void myDecodeFun();
void myCleanUpFun();
void keyCallback(int key, int, int action, int);
void contextCreationCallback(GLFWwindow * win);

//drag and drop files to window
void myDropCallback(int count, const char** paths);

void myDataTransferDecoder(void * receivedData, int receivedlength, int packageId, int clientIndex);
void myDataTransferStatus(bool connected, int clientIndex);
void myDataTransferAcknowledge(int packageId, int clientIndex);

void startDataTransfer();
void readImage(unsigned char * data, int len);
void uploadTexture();
void threadWorker();

std::thread * loadThread;
std::mutex mutex;
GLFWwindow * hiddenWindow;
GLFWwindow * sharedWindow;
std::vector<sgct::core::Image*> transImages;

//sync variables
sgct::SharedBool info(false);
sgct::SharedBool stats(false);
sgct::SharedBool wireframe(false);
sgct::SharedInt32 texIndex(-1);
sgct::SharedInt32 incrIndex(1);
sgct::SharedInt32 numSyncedTex(0);

//other mutex variables
sgct::SharedInt32 lastPackage(-1);
sgct::SharedBool running(true);
sgct::SharedBool transfer(false);
sgct::SharedBool serverUploadDone(false);
sgct::SharedInt32 serverUploadCount(0);
sgct::SharedBool clientsUploadDone(false);
sgct::SharedVector<std::pair<std::string, int>> imagePaths;
sgct::SharedVector<GLuint> texIds;
double sendTimer = 0.0;

sgct::SharedFloat domeDiameter(14.8f);
sgct::SharedFloat domeTilt(-27.0f);

enum imageType { IM_JPEG, IM_PNG };
const int headerSize = 1;
sgct::utils::Dome * dome = NULL;
GLint Matrix_Loc = -1;

//variables to share across cluster
sgct::SharedDouble curr_time(0.0);

using namespace sgct;

int main( int argc, char* argv[] )
{
    std::vector<std::string> arg(argv + 1, argv + argc);
    Configuration config = parseArguments(arg);
    config::Cluster cluster = loadCluster(config.configFilename);
    gEngine = new Engine(config);
    
    gEngine->setInitOGLFunction( myInitOGLFun );
    gEngine->setPreSyncFunction( myPreSyncFun );
    gEngine->setPostSyncPreDrawFunction( myPostSyncPreDrawFun );
    gEngine->setDrawFunction( myDrawFun );
    gEngine->setPostDrawFunction(myPostDrawFun);
    gEngine->setCleanUpFunction( myCleanUpFun );
    gEngine->setKeyboardCallbackFunction(keyCallback);
    gEngine->setContextCreationCallback( contextCreationCallback );
    gEngine->setDropCallbackFunction(myDropCallback);

    if (!gEngine->init(Engine::RunMode::OpenGL_3_3_Core_Profile, cluster)) {
        delete gEngine;
        return EXIT_FAILURE;
    }
    
    gEngine->setDataTransferCallback(myDataTransferDecoder);
    gEngine->setDataTransferStatusCallback(myDataTransferStatus);
    gEngine->setDataAcknowledgeCallback(myDataTransferAcknowledge);
    //gEngine->setDataTransferCompression(true, 6);

    //sgct::SharedData::instance()->setCompression(true);
    sgct::SharedData::instance()->setEncodeFunction(myEncodeFun);
    sgct::SharedData::instance()->setDecodeFunction(myDecodeFun);

    // Main loop
    gEngine->render();

    // Clean up OpenVR
    sgct::openvr::shutdown();

    running.setVal(false);

    if (loadThread)
    {
        loadThread->join();
        delete loadThread;
    }

    // Clean up
    delete gEngine;

    // Exit program
    exit( EXIT_SUCCESS );
}

void myInitOGLFun()
{
    //Find if we have at least one OpenVR window
    //Save reference to first OpenVR window, which is the one we will copy to the HMD.
    for (size_t i = 0; i < gEngine->getNumberOfWindows(); i++) {
        if (gEngine->getWindow(i).hasTag("OpenVR")) {
            FirstOpenVRWindow = &gEngine->getWindow(i);
            break;
        }
    }
    //If we have an OpenVRWindow, initialize OpenVR.
    if (FirstOpenVRWindow) {
        sgct::MessageHandler::printError("OpenVR Initalized");
        sgct::openvr::initialize(gEngine->getNearClippingPlane(), gEngine->getFarClippingPlane());
    }

    dome = new utils::Dome(domeDiameter.getVal()*0.5f, 180.0f, 256, 128);

    //Set up backface culling
    glCullFace(GL_BACK);
    glFrontFace(GL_CW); //our polygon winding is clockwise since we are inside of the dome

    sgct::ShaderManager::instance()->addShaderProgram(
        "xform",
        vertexShader,
        fragmentShader,
        ShaderProgram::ShaderSourceType::String
    );
    sgct::ShaderManager::instance()->bindShaderProgram("xform");

    Matrix_Loc = sgct::ShaderManager::instance()->getShaderProgram("xform").getUniformLocation("mvp");
    GLint Tex_Loc = sgct::ShaderManager::instance()->getShaderProgram("xform").getUniformLocation("tex");
    glUniform1i(Tex_Loc, 0);

    sgct::ShaderManager::instance()->unBindShaderProgram();
}

void myPreSyncFun()
{
    if (gEngine->isMaster())
    {
        curr_time.setVal(sgct::Engine::getTime());

        //if texture is uploaded then iterate the index
        if (serverUploadDone.getVal() && clientsUploadDone.getVal())
        {
            numSyncedTex = static_cast<int32_t>(texIds.getSize());

            //only iterate up to the first new image, even if multiple images was added
            texIndex = numSyncedTex.getVal() - serverUploadCount.getVal();

            serverUploadDone = false;
            clientsUploadDone = false;
        }
    }
}

void myPostSyncPreDrawFun()
{
    if (FirstOpenVRWindow) {
        //Update pose matrices for all tracked OpenVR devices once per frame
        sgct::openvr::updatePoses();
    }

    gEngine->setDisplayInfoVisibility(info.getVal());
    gEngine->setStatsGraphVisibility(stats.getVal());
    gEngine->setWireframe(wireframe.getVal());
}

void myPreDrawFun()
{
    if (FirstOpenVRWindow) {
        //Update pose matrices for all tracked OpenVR devices once per frame
        sgct::openvr::updatePoses();
    }
}

void myDrawFun()
{
    if (texIndex.getVal() != -1)
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        glm::mat4 MVP;
        if (sgct::openvr::isHMDActive() &&
            (FirstOpenVRWindow == &gEngine->getCurrentWindow() || gEngine->getCurrentWindow().hasTag("OpenVR"))) {
            MVP = sgct::openvr::getHMDCurrentViewProjectionMatrix(gEngine->getCurrentFrustumMode());

            if (gEngine->getCurrentFrustumMode() == core::Frustum::Mode::MonoEye) {
                //Reversing rotation around z axis (so desktop view is more pleasent to look at).
                glm::quat inverserotation = sgct::openvr::getInverseRotation(sgct::openvr::getHMDPoseMatrix());
                inverserotation.x = inverserotation.y = 0.f;
                MVP *= glm::mat4_cast(inverserotation);
            }

            //Tilt dome
            glm::mat4 tiltMat = glm::mat4(glm::rotate(glm::mat4(1.0f), glm::radians(domeTilt.getVal()), glm::vec3(1.0f, 0.0f, 0.0f)));
            MVP *= tiltMat;
        }
        else {
            MVP = gEngine->getCurrentModelViewProjectionMatrix();
        }
        
        glActiveTexture(GL_TEXTURE0);

        if ((texIds.getSize() > (texIndex.getVal() + 1))
            && gEngine->getCurrentFrustumMode() == core::Frustum::Mode::StereoRightEye){
            glBindTexture(GL_TEXTURE_2D, texIds.getValAt(texIndex.getVal()+1));
        }
        else{
            glBindTexture(GL_TEXTURE_2D, texIds.getValAt(texIndex.getVal()));
        }

        sgct::ShaderManager::instance()->bindShaderProgram("xform");
        glUniformMatrix4fv(Matrix_Loc, 1, GL_FALSE, &MVP[0][0]);

        //draw the box
        dome->draw();

        sgct::ShaderManager::instance()->unBindShaderProgram();

        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
    }
}

void myPostDrawFun()
{
    if (FirstOpenVRWindow) {
        //Copy the first OpenVR window to the HMD
        sgct::openvr::copyWindowToHMD(FirstOpenVRWindow);
    }
}

void myEncodeFun()
{
    SharedData::instance()->writeDouble(curr_time);
    SharedData::instance()->writeBool(info);
    SharedData::instance()->writeBool(stats);
    SharedData::instance()->writeBool(wireframe);
    SharedData::instance()->writeInt32(texIndex);
    SharedData::instance()->writeInt32(incrIndex);
}

void myDecodeFun()
{
    SharedData::instance()->readDouble(curr_time);
    SharedData::instance()->readBool(info);
    SharedData::instance()->readBool(stats);
    SharedData::instance()->readBool(wireframe);
    SharedData::instance()->readInt32(texIndex);
    SharedData::instance()->readInt32(incrIndex);
}

void myCleanUpFun()
{
    if (dome != NULL)
        delete dome;
    
    for(std::size_t i=0; i < texIds.getSize(); i++)
    {
        GLuint tex = texIds.getValAt(i);
        if (tex)
        {
            glDeleteTextures(1, &tex);
            texIds.setValAt(i, 0);
        }
    }
    texIds.clear();
    
    
    if(hiddenWindow)
        glfwDestroyWindow(hiddenWindow);
}

void keyCallback(int key, int, int action, int) {
    if (gEngine->isMaster()) {
        switch (key) {
            case key::S:
                if (action == action::Press) {
                    stats.setVal(!stats.getVal());
                }
                break;
            case key::I:
                if (action == action::Press) {
                    info.setVal(!info.getVal());
                }
                break;
            case key::W:
                if (action == action::Press) {
                    wireframe.setVal(!wireframe.getVal());
                }
                break;

            case key::F:
                if (action == action::Press) {
                    wireframe.setVal(!wireframe.getVal());
                }
                break;
            case key::Key1:
                if (action == action::Press) {
                    incrIndex.setVal(1);
                }
                break;
            case key::Key2:
                if (action == action::Press) {
                    incrIndex.setVal(2);
                }
                break;
            case key::Left:
                if (action == action::Press && numSyncedTex.getVal() > 0) {
                    texIndex.getVal() > incrIndex.getVal() - 1 ? texIndex.setVal(texIndex.getVal()-incrIndex.getVal()) : texIndex.setVal(numSyncedTex.getVal() - 1);
                    //fprintf(stderr, "Index set to: %d\n", texIndex.getVal());
                }
                break;
            case key::Right:
                if (action == action::Press && numSyncedTex.getVal() > 0) {
                    texIndex.setVal((texIndex.getVal() + incrIndex.getVal()) % numSyncedTex.getVal());
                    //fprintf(stderr, "Index set to: %d\n", texIndex.getVal());
                }
                break;
            case key::Up:
                domeTilt.setVal(domeTilt.getVal() + 0.1f);
                break;
            case key::Down:
                domeTilt.setVal(domeTilt.getVal() - 0.1f);
                break;
        }
    }
}

void contextCreationCallback(GLFWwindow * win)
{
    glfwWindowHint( GLFW_VISIBLE, GLFW_FALSE );
    
    sharedWindow = win;
    hiddenWindow = glfwCreateWindow( 1, 1, "Thread Window", NULL, sharedWindow );
     
    if( !hiddenWindow )
    {
        sgct::MessageHandler::printInfo("Failed to create loader context");
    }
    
    //restore to normal
    glfwMakeContextCurrent( sharedWindow );
    
    if( gEngine->isMaster() )
        loadThread = new (std::nothrow) std::thread(threadWorker);
}

void myDataTransferDecoder(void * receivedData, int receivedlength, int packageId, int clientIndex)
{
    sgct::MessageHandler::printInfo("Decoding %d bytes in transfer id: %d on node %d", receivedlength, packageId, clientIndex);

    lastPackage.setVal(packageId);
    
    //read the image on slave
    readImage( reinterpret_cast<unsigned char*>(receivedData), receivedlength);
    uploadTexture();
}

void myDataTransferStatus(bool connected, int clientIndex)
{
    sgct::MessageHandler::printInfo("Transfer node %d is %s", clientIndex, connected ? "connected" : "disconnected");
}

void myDataTransferAcknowledge(int packageId, int clientIndex)
{
    sgct::MessageHandler::printInfo("Transfer id: %d is completed on node %d", packageId, clientIndex);
    
    static int counter = 0;
    if( packageId == lastPackage.getVal())
    {
        counter++;
        if( counter == (core::ClusterManager::instance()->getNumberOfNodes()-1) )
        {
            clientsUploadDone = true;
            counter = 0;
            
            sgct::MessageHandler::printInfo("Time to distribute and upload textures on cluster: %f ms\n", (sgct::Engine::getTime() - sendTimer)*1000.0);
        }
    }
}

void threadWorker()
{
    while (running.getVal())
    {
        //runs only on master
        if (transfer.getVal() && !serverUploadDone.getVal() && !clientsUploadDone.getVal())
        {
            startDataTransfer();
            transfer.setVal(false);
            
            //load textures on master
            uploadTexture();
            serverUploadDone = true;
            
            if(core::ClusterManager::instance()->getNumberOfNodes() == 1) //no cluster
            {
                clientsUploadDone = true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); //ten iteration per second
    }
}

void startDataTransfer()
{
    //iterate
    int id = lastPackage.getVal();
    id++;

    //make sure to keep within bounds
    if(static_cast<int>(imagePaths.getSize()) > id)
    {
        sendTimer = sgct::Engine::getTime();

        int imageCounter = static_cast<int32_t>(imagePaths.getSize());
        lastPackage.setVal(imageCounter - 1);

        for (int i = id; i < imageCounter; i++)
        {
            //load from file
            std::pair<std::string, int> tmpPair = imagePaths.getValAt(static_cast<std::size_t>(i));

            std::ifstream file(tmpPair.first.c_str(), std::ios::binary);
            file.seekg(0, std::ios::end);
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);

            std::vector<char> buffer(size + headerSize);
            char type = tmpPair.second;

            //write header (single unsigned char)
            buffer[0] = type;

            if (file.read(buffer.data() + headerSize, size))
            {
                //transfer
                gEngine->transferDataBetweenNodes(buffer.data(), static_cast<int>(buffer.size()), i);

                //read the image on master
                readImage(reinterpret_cast<unsigned char *>(buffer.data()), static_cast<int>(buffer.size()));
            }
        }
    }
}

void readImage(unsigned char * data, int len)
{
    mutex.lock();
    
    core::Image * img = new (std::nothrow) core::Image();
    
    char type = static_cast<char>(data[0]);
    
    bool result = false;
    switch( type )
    {
        case IM_JPEG:
            result = img->loadJPEG(reinterpret_cast<unsigned char*>(data + headerSize), len - headerSize);
            break;
            
        case IM_PNG:
            result = img->loadPNG(reinterpret_cast<unsigned char*>(data + headerSize), len - headerSize);
            break;
    }
    
    if (!result)
    {
        //clear if failed
        delete img;
    }
    else
        transImages.push_back(img);

    mutex.unlock();
}

void uploadTexture()
{
    mutex.lock();

    if (!transImages.empty())
    {
        glfwMakeContextCurrent(hiddenWindow);

        for (std::size_t i = 0; i < transImages.size(); i++)
        {
            if (transImages[i])
            {
                //create texture
                GLuint tex;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

                GLenum internalformat;
                GLenum type;
                unsigned int bpc = transImages[i]->getBytesPerChannel();

                switch (transImages[i]->getChannels())
                {
                case 1:
                    internalformat = (bpc == 1 ? GL_R8 : GL_R16);
                    type = GL_RED;
                    break;

                case 2:
                    internalformat = (bpc == 1 ? GL_RG8 : GL_RG16);
                    type = GL_RG;
                    break;

                case 3:
                default:
                    internalformat = (bpc == 1 ? GL_RGB8 : GL_RGB16);
                    type = GL_BGR;
                    break;

                case 4:
                    internalformat = (bpc == 1 ? GL_RGBA8 : GL_RGBA16);
                    type = GL_BGRA;
                    break;
                }

                GLenum format = (bpc == 1 ? GL_UNSIGNED_BYTE : GL_UNSIGNED_SHORT);

                glTexStorage2D(GL_TEXTURE_2D, 1, internalformat, transImages[i]->getWidth(), transImages[i]->getHeight());
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, transImages[i]->getWidth(), transImages[i]->getHeight(), type, format, transImages[i]->getData());

                //---------------------
                // Disable mipmaps
                //---------------------
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                //unbind
                glBindTexture(GL_TEXTURE_2D, 0);

                sgct::MessageHandler::printInfo("Texture id %d loaded (%dx%dx%d)", tex, transImages[i]->getWidth(), transImages[i]->getHeight(), transImages[i]->getChannels());

                texIds.addVal(tex);

                delete transImages[i];
                transImages[i] = NULL;
            }
            else //if invalid load
            {
                texIds.addVal(0);
            }
        }//end for

        transImages.clear();
        glFinish();

        //restore
        glfwMakeContextCurrent(NULL);
    }//end if not empty

    mutex.unlock();
}

void myDropCallback(int count, const char** paths)
{
    if (gEngine->isMaster())
    {
        std::vector<std::string> pathStrings;
        for (int i = 0; i < count; i++)
        {
            //simpy pick the first path to transmit
            std::string tmpStr(paths[i]);

            //transform to lowercase
            std::transform(tmpStr.begin(), tmpStr.end(), tmpStr.begin(), ::tolower);

            pathStrings.push_back(tmpStr);
        }

        //sort in alphabetical order
        std::sort(pathStrings.begin(), pathStrings.end());

        serverUploadCount.setVal(0);

        //iterate all drop paths
        for (int i = 0; i < pathStrings.size(); i++)
        {
            std::size_t found;

            std::string tmpStr = pathStrings[i];

            //find file type
            found = tmpStr.find(".jpg");
            if (found != std::string::npos)
            {
                imagePaths.addVal(std::pair<std::string, int>(pathStrings[i], IM_JPEG));
                transfer.setVal(true); //tell transfer thread to start processing data
                serverUploadCount.setVal(serverUploadCount.getVal()+1);
            }
            else
            {
                found = tmpStr.find(".jpeg");
                if (found != std::string::npos)
                {
                    imagePaths.addVal(std::pair<std::string, int>(pathStrings[i], IM_JPEG));
                    transfer.setVal(true); //tell transfer thread to start processing data
                    serverUploadCount.setVal(serverUploadCount.getVal() + 1);
                }
                else
                {
                    found = tmpStr.find(".png");
                    if (found != std::string::npos)
                    {
                        imagePaths.addVal(std::pair<std::string, int>(pathStrings[i], IM_PNG));
                        transfer.setVal(true); //tell transfer thread to start processing data
                        serverUploadCount.setVal(serverUploadCount.getVal() + 1);
                    }
                }
            }
        }
    }
}
