/*
neural2d.h
David R. Miller, 2014
See https://github.com/davidrmiller/neural2d for more information.

For more information, see the tutorial video.
*/

/*
 * Notes:
 *
 * This is a backpropagation neural net simulator with these features:
 *
 *    1. Optimized for 2D image data -- input data is read from .bmp image files
 *    2. Neuron layers can be abstracted as 1D or 2D arrangements of neurons
 *    3. Network topology is defined in a text file
 *    4. Neurons in layers can be fully or sparsely connected
 *    5. Selectable transfer function per layer
 *    6. Adjustable or automatic training rate (eta)
 *    7. Optional momentum (alpha) and regularization (lambda)
 *    8. Standalone console program
 *    9. Heavily-commented code, < 2000 lines, suitable for prototyping, learning, and experimentation
 *   10. Optional web GUI controller
 *   11. Tutorial video coming soon!
 *
 * This program is written in the C++-11 dialect. It uses mostly ISO-standard C++
 * features and a few POSIX features that should be widely available on any
 * compiler that knows about C++11.
 *
 * This is a console program that requires no GUI. However, a GUI is optional:
 * The WebServer class provides an HTTP server that a web browser can connect to
 * at port 24080.
 *
 * For most array indices and offsets into containers, we use 32-bit integers
 * (uint32_t). This allows 2 billion neurons per layer, 2 billion connections
 * total, etc.
 *
 * For training input samples, we use BMP images because we can read those
 * easily with standard C/C++ without using image libraries. The function ReadBMP()
 * reads an image file and converts the pixel data into an array (container) of
 * doubles. When an image is first read, we'll cache the image data in memory to
 * make subsequent reads faster (because we may want to input the training samples
 * multiple times during training).
 *
 * There are three common modes of operation:
 *
 *     TRAINING: input samples are labeled with target output values, and
 *               weights are adjusted during training. For this mode,
 *               call feedForward(), backProp(), and reportResults()
 *               once per input sample. Call saveWeights() once the
 *               net is trained.
 *
 *     VALIDATE: input samples are labeled with target output values,
 *               output values are reported, but weights are NOT adjusted.
 *               For this mode, call loadWeights(), then call
 *               feedForward() and reportResults() once per input sample.
 *
 *     TRAINED:  input samples have no target output values; outputs are
 *               reported, weights are NOT adjusted. For this mode,
 *               call loadWeights(), then call feedForward() and
 *               reportResults() once per input sample.
 *
 * Typical operation is to label a bunch of input samples, use some of them in
 * TRAINING mode, and save the weights when the net is trained. Then using the saved
 * weights and some more labeled training samples, test how well the net performs
 * in VALIDATE mode. If successful, the saved weights can be used in TRAINED
 * mode.
 *
 * Class relationships: Everything is in the NNet namespace. Class Net can be
 * instantiated to create a neural net. The Net object holds a container of
 * Layer objects. Each Layer object holds a container of Neuron objects. Each
 * Neuron object holds containers of references to Connection objects which
 * define the connections. The container of Connection objects is held in the
 * net object and neurons refer to connections by indices. Class SampleSet holds
 * the input samples that are presented to the neural net when the
 * feedForward() member is called.
 */

#ifndef NNET_H
#define NNET_H

// ISO-standard C++ headers:

#include <algorithm>
#include <cassert>
#include <condition_variable> // For mutex
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility> // For pair(), make_pair()
#include <vector>

using namespace std;

// POSIX headers:

#include <unistd.h>    // For sleep()

#ifdef _WIN32
// Windows headers:
#include <windows.h>
#define sleep(secs) Sleep(secs * 1000)
#endif

// For web server:
#include <sys/socket.h>
#include <netinet/in.h>


// Everything we define in this file will be inside the NNet namespace. This keeps
// all of our definitions out of the global namespace. (You can indent all the source
// lines inside the namespace below if you're an indenting purist.)
//
namespace NNet {

//  ****************************  For the web server interface  *****************************

// A Thread-safe FIFO; pushes to the back, pops from the front. Push and
// pop are always non-blocking. If the queue is empty, pop() immediately
// returns with s set to an empty string.

struct Message_t
{
    string text;
    int httpResponseFileDes;
};

class MessageQueue
{
public:
    MessageQueue() { };
    void push(Message_t msg);
    void pop(Message_t &msg);
    MessageQueue(const MessageQueue &) = delete;            // No copying
    MessageQueue &operator=(const MessageQueue &) = delete; // No assignment

private:
    queue<Message_t> mqueue;
    mutex mmutex;
};


class WebServer
{
public:
    WebServer(void);
    ~WebServer(void);
    void start(int portNumber, MessageQueue &messages);
    void stopServer(void);
    void sendHttpResponse(string parameterBlock, int httpResponseFileDes);
    void webServerThread(int portNumber, MessageQueue &messageQueue);
    int portNumber;    // needed?
    int socketFd;

private:
    void initializeHttpResponse(void);
    void extractAndQueueMessage(string s, int httpConnectionFd, MessageQueue &messages);
    void replyToUnknownRequest(int httpConnectionFd);

    string firstPart;  // First part of the HTTP response
    string secondPart; // Last part of the HTTP response
};


//  ***********************************  Input samples  ***********************************

// Training samples, including the pixel data. Each sample consists of an input
// filename and expected output values. When running a trained net on unlabeled
// input data, the target values are not known and therefore not used.
//
// The code below is specific for samples derived from image files. The parameter
// Net::colorChannel can be set to choose the function that converts the RGB pixel
// value into a floating point number in the range 0.0..1.0.
//

enum ColorChannel_t { R, G, B, BW };


class Sample
{
public:
    string imageFilename;
    vector<double> &getData(ColorChannel_t colorChannel);
    void clearPixelCache(void);
    vector<double> targetVals;

private:
    vector<double> data; // Pixel data converted to doubles and flattened to a 1D array
};


class SampleSet
{
public:
    SampleSet() {};
    void loadSamples(const string &inputDataConfigFilename);
    void shuffle(void);
    void clearPixelCache(void);
    vector<Sample> samples;
};


class Neuron; // Forward reference

typedef vector<double> matColumn_t;
typedef vector<matColumn_t> convolveMatrix_t; // Allows access as convolveMatrix[x][y]

typedef double (*transferFunction_t)(double); // Also used for the derivative function

struct layerParams_t {
    layerParams_t() { clear(); }
    void resolveTransferFunctionName(void);
    void clear(void);
    string layerName;                  // Can be input, output, or layer*
    string fromLayerName;              // Can be any existing layer name
    uint32_t sizeX;                    // Format: size XxY
    uint32_t sizeY;
    ColorChannel_t channel = NNet::BW; // Applies only to the input layer
    bool colorChannelSpecified;
    uint32_t radiusX = 1e9;            // Format: radius XxY
    uint32_t radiusY = 1e9;
    string transferFunctionName = "";  // Format: tf name
    transferFunction_t tf;
    transferFunction_t tfDerivative;
    convolveMatrix_t convolveMatrix;   // Format: convolve {{0,1,0},...
    bool isConvolutionLayer;           // Equivalent to (convolveMatrix.size() != 0)
};


//  ***********************************  class Layer  ***********************************

// Each layer is a bag of neurons in a 2D arrangement:
//
struct Layer
{
    layerParams_t params;

    // For each layer, before any references are made to its members, the .neurons
    // member must be initialized with sufficient capacity to prevent reallocation.
    // This allows us to form stable pointers, iterators, or references to neurons.
    vector<Neuron> neurons;  // 2d array, flattened index = y * sizeX + x
};


// ***********************************  class Connection  ***********************************

// If neurons are considered as nodes in a directed graph, the edges would be the
// "connections". Each connection goes from one neuron to any other neuron in any
// layer. Connections are analogous to synapses in natural biology. The .weight
// member is what it's all about -- once a net is trained, we only need to save
// the weights of all the connections. The set of all weights plus the network
// topology defines the neural net's function. The .deltaWeight member is used
// only for the momentum calculation.
//
struct Connection
{
    Connection(Neuron &from, Neuron &to);
    Neuron &fromNeuron;
    Neuron &toNeuron;
    double weight;
    double deltaWeight;  // The weight change from the previous training iteration
};


// ***********************************  class Neuron  ***********************************

class Neuron
{
public:
    Neuron();
    Neuron(vector<Connection> *pConnectionsData, transferFunction_t tf, transferFunction_t tfDerivative);
    double output;
    double gradient;

    // All the input and output connections for this neuron. We store these as indices
    // into an array of Connection objects stored somewhere else. We store indices
    // instead of pointers or iterators because the container of Connection objects
    // can get reallocated which would invalidate the references. Each neuron gets
    // a pointer to the connections container. Currently the connections container
    // is a public data member of class Net, but it could be stored anywhere that is
    // accessible.

    vector<Connection> *pConnections;   // Pointer to the container of Connection records
    vector<uint32_t> backConnectionsIndices;
    vector<uint32_t> forwardConnectionsIndices; // Refers to another neuron's back connections

    void feedForward(void);                     // Propagate the net inputs to the outputs
    void updateInputWeights(double eta, double alpha);  // For backprop training
    void calcOutputGradients(double targetVal);         // For backprop training
    void calcHiddenGradients(void);                     // For backprop training

    // The only reason for the .sourceNeurons member is to make it easy to
    // find and report any unconnected neurons. For everything else, we'll use
    // the backConnectionIndices to find the neuron's inputs. This container
    // holds pointers to all the source neurons that feed this neuron:

    set<Neuron *> sourceNeurons;

private:
    double (*transferFunction)(double x);
    double (*transferFunctionDerivative)(double x);
    double sumDOW_nextLayer(void) const;        // Used in hidden layer backprop training
};


// ***********************************  class Net  ***********************************

class Net
{
public:
    // Parameters that affect overall network operation. These can be set by
    // directly accessing the data members:

    // The color channel specifies how RGB pixels are converted to doubles:

    ColorChannel_t colorChannel;   // R, G, B, or BW

    bool enableBackPropTraining; // If false, backProp() won't update any weights

    // Training will pause when the recent average overall error falls below this threshold:

    double doneErrorThreshold;

    // eta is the network learning rate. It can be set to a constant value, somewhere
    // in the range 0.0001 to 0.1. Optionally, set dynamicEtaAdjust to true to allow
    // the program to automatically adjust eta during learning for optimal learning.

    double eta;               // Initial overall net learning rate, [0.0..1.0]
    bool dynamicEtaAdjust;    // true enables automatic eta adjustment during training

    // alpha is the momentum factor. Set it to zero to disable momentum. If alpha > 0, then
    // changes in any connection weight in the same direction that the weight was changed
    // last time is amplified. This helps converge on a solution a little faster during
    // the early stages of training, but if set too high will interfere with the network
    // converging on the most accurate solution.

    double alpha;              // Initial momentum, multiplier of last deltaWeight, [0.0..1.0]

    // Regularization parameter. If zero, regularization is disabled:

    double lambda;

    // When a net topology specifies sparse connections (i.e., when there is a radius
    // parameter specified in the topology config file), then the shape of the area
    // that is back-projected onto the source layer of neurons can be elliptical or
    // rectangular. The default is elliptical (false).

    bool projectRectangular;

    bool isRunning;     // If true, start processing without waiting for a "resume" command
    
    // To reduce screen clutter during training, reportEveryNth can be set > 1. When
    // in VALIDATE or TRAINED mode, you'll want to set this to 1 so you can see every
    // result:

    uint32_t reportEveryNth;

    // For some calculations, we use a running average of net error, averaged over
    // this many input samples:

    double recentAverageSmoothingFactor;

    // If repeatInputSamples is false, the program will pause after running all the
    // input samples once. If set to true, the input samples will automatically repeat.
    // If shuffleInputSamplies is true, then the input samples will be randomly
    // shuffled after each use:

    bool repeatInputSamples;
    bool shuffleInputSamples;

    string weightsFilename;     // Filename to use in saveWeights() and loadWeights()

    uint32_t inputSampleNumber; // Increments each time feedForward() is called
    double error;               // Overall net error
    double recentAverageError;  // Averaged over recentAverageSmoothingFactor samples

    // Creates and connects a net from a topology config file:

    Net(const string &topologyFilename);
    ~Net(void);

    void feedForward(void);                       // Propagate inputs to outputs
    void feedForward(Sample &sample);
    void backProp(const Sample &sample);          // Backprop and update all weights

    // The connection weights can be saved or restored at any time. Note that the network
    // topology is not saved in the weights file, so you'll have to manually keep track of
    // which weights file goes with which topology file.

    bool saveWeights(const string &filename) const;
    bool loadWeights(const string &filename);

    // Functions for forward propagation:

    double getNetError(void) const { return error; };
    double getRecentAverageError(void) const { return recentAverageError; };
    void calculateOverallNetError(const Sample &sample);  // Update .error member

    // Functions for displaying the results when processing input samples:

    void reportResults(const Sample &sample) const;
    void debugShowNet(bool details = false);      // Display details of net topology

    SampleSet sampleSet;     // List of input images and access to their data

private:
    // Here is where we store all the weighted connections. The container can get
    // reallocated, so we'll only refer to elements by indices, not by pointers or
    // references. This also allows us to hack on the connections container during
    // training if we want to, by dynamically adding or deleting connections without
    // invalidating references.

    vector<Connection> connections;

    Neuron bias;  // Fake neuron with constant output 1.0
    void initTrainingSamples(const string &inputFilename);
    void parseConfigFile(const string &configFilename); // Creates layer metadata from a config file
    convolveMatrix_t parseMatrixSpec(istringstream &ss);
    Layer &createLayer(const layerParams_t &params);
    bool addToLayer(Layer &layerTo, Layer &layerFrom, layerParams_t &params);
    void createNeurons(Layer &layerFrom, Layer &layerTo, layerParams_t &params);
    void connectNeuron(Layer &layerFrom, Layer &layerTo, Neuron &neuron,
                       uint32_t nx, uint32_t ny, layerParams_t &params);
    void connectBias(Neuron &neuron);
    int32_t getLayerNumberFromName(const string &name) const;

    void doCommand(); // Handles incoming program command and control
    void actOnMessageReceived(Message_t &msg);
    double adjustedEta(void);

    vector<Layer> layers;

    double lastRecentAverageError;   // Used for dynamically adjusting eta
    uint32_t totalNumberConnections; // Including 1 bias connection per neuron
    uint32_t totalNumberNeurons;
    double sumWeights;               // For regularization calculation

    // Stuff for the web interface:

    WebServer webServer;
    bool enableWebServer;
    int portNumber;
    MessageQueue messages;
    void makeParameterBlock(string &s);
};

} // end namespace NNet

#endif // end #ifndef NNET_H
