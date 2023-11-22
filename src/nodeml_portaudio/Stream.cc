#include <nodeml_portaudio/Stream.h>
#include <nodeml_portaudio/utils.h>
#include <future>
#include "Stream.h"
#include <iostream>

namespace nodeml_portaudio
{

    Napi::FunctionReference Stream::constructor;

    Napi::Object Stream::Init(Napi::Env env, Napi::Object exports)
    {
        auto func = DefineClass(env, "Stream",
                                {Stream::StaticMethod("create", &Stream::Create),
                                 Stream::InstanceMethod("start", &Stream::Start),
                                 Stream::InstanceMethod("stop", &Stream::Stop),
                                 Stream::InstanceMethod("close", &Stream::Close)});

        constructor = Napi::Persistent(func);
        constructor.SuppressDestruct();
        exports.Set("Stream", func);
        return exports;
    }

    Stream::Stream(const Napi::CallbackInfo &info) : ObjectWrap(info)
    {
        stream = NULL;
        streamInfo = NULL;
    }

    Stream *Stream::FromObject(Napi::Value value)
    {
        return Napi::ObjectWrap<Stream>::Unwrap(value.ToObject());
    }

    Napi::Value Stream::Create(const Napi::CallbackInfo &info)

    {
        auto env = info.Env();

        try
        {
            Napi::EscapableHandleScope scope(env);

            auto newStream = Stream::constructor.New({});

            auto unwraped = Napi::ObjectWrap<Stream>::Unwrap(newStream);

            unwraped->prepare(info);

            return scope.Escape(newStream).ToObject();
        }
        catch (const std::exception &e)
        {
            throw Napi::Error::New(env, e.what());
        }
    }
    Napi::Value Stream::IsActive(const Napi::CallbackInfo &info)
    {
        auto env = info.Env();

        try
        {
            return Napi::Boolean::New(env, Pa_IsStreamActive(stream));
        }
        catch (const std::exception &e)
        {
            throw Napi::Error::New(env, e.what());
        }
    }
    Napi::Value Stream::Start(const Napi::CallbackInfo &info)
    {
        auto env = info.Env();

        auto startResult = Pa_StartStream(stream);

        if (startResult != paNoError)
        {
            throw Napi::Error::New(env, Pa_GetErrorText(startResult));
        }

        return env.Undefined();
    }
    Napi::Value Stream::Stop(const Napi::CallbackInfo &info)
    {
        auto env = info.Env();

        auto stopResult = Pa_StopStream(stream);

        if (stopResult != paNoError)
        {
            throw Napi::Error::New(env, Pa_GetErrorText(stopResult));
        }

        try
        {
            cleanup();
        }
        catch (const std::exception &e)
        {
            throw Napi::Error::New(env, e.what());
        }

        return env.Undefined();
    }

    Napi::Value Stream::Close(const Napi::CallbackInfo &info)
    {
        auto env = info.Env();

        auto closeResult = Pa_CloseStream(stream);

        if (closeResult != paNoError)
        {
            throw Napi::Error::New(env, Pa_GetErrorText(closeResult));
        }

        stream = NULL;

        try
        {
            cleanup();
        }
        catch (const std::exception &e)
        {
            throw Napi::Error::New(env, e.what());
        }

        return env.Undefined();
    }

    void Stream::prepare(const Napi::CallbackInfo &info)
    {
        auto env = info.Env();

        try
        {
            PaStreamParameters inputParams;
            PaStreamParameters outputParams;
            inputParams.device = paNoDevice;
            outputParams.device = paNoDevice;

            if (info[0].IsObject())
                utils::GetStreamParameters(inputParams, info[0].ToObject());

            if (info[1].IsObject())
                utils::GetStreamParameters(outputParams, info[1].ToObject());

            if (inputParams.device == outputParams.device && inputParams.device == paNoDevice)
            {
                throw Napi::Error::New(env, "Cannot open a stream with no input or output");
            }

            auto sampleRate = info[2].ToNumber().DoubleValue();

            auto framesPerBuffer = info[3].ToNumber().Int64Value();

            auto callback = info[4].As<Napi::Function>();

            cleanup();

            streamInfo = new StreamInfo();

            streamInfo->tsfn = Napi::ThreadSafeFunction::New(
                env, callback, "StreamCallback", 0, 1, [](Napi::Env env, StreamInfo *pAudioDataCreated)
                { delete pAudioDataCreated; },
                streamInfo);

            streamInfo->bHasBeenAquired = false;

            if (inputParams.device == outputParams.device)
            {
                streamInfo->type = EStreamType::Duplex;
            }
            else if (inputParams.device != paNoDevice)
            {
                streamInfo->type = EStreamType::Input;
            }
            else
            {
                streamInfo->type = EStreamType::Output;
            }

            streamInfo->inputParams = inputParams;
            streamInfo->outputParams = outputParams;

            auto openResult = Pa_OpenStream(
                &stream, inputParams.device == paNoDevice ? NULL : &inputParams, outputParams.device == paNoDevice ? NULL : &outputParams,
                sampleRate,
                framesPerBuffer,
                paNoFlag, [](const void *input, void *output, unsigned long frameCount, const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData)
                { 

                    auto owner = (Stream *)userData;

                    if(!owner->streamInfo->bHasBeenAquired){
                        owner->streamInfo->tsfn.Acquire();
                        owner->streamInfo->bHasBeenAquired = true;
                    }
                    std::promise<int> promise;

                    auto future = promise.get_future();

                    StreamCallbackInfo * info = new StreamCallbackInfo();

                    info->dataIn = utils::CopyInputData(input,frameCount * owner->streamInfo->inputParams.channelCount,owner->streamInfo->inputParams.sampleFormat);
                    info->dataOut = output;
                    info->frameCount = frameCount;
                    info->timeInfo = timeInfo;
                    info->streamInfo = owner->streamInfo;

                    owner->streamInfo->tsfn.BlockingCall(info,[&promise](Napi::Env env, Napi::Function jsCallback, StreamCallbackInfo * value)
                    {

                        auto dataInputJs = env.Undefined();

                        if(value->dataIn != nullptr){
                            dataInputJs = utils::CopyToTypedArray(env,value->dataIn,value->frameCount * value->streamInfo->inputParams.channelCount,value->streamInfo->inputParams.sampleFormat);
                            delete[] value->dataIn;
                            
                            value->dataIn = nullptr;
                            
                        }
                         

                        auto dataOutputJs = env.Undefined();
                        if(value->streamInfo->outputParams.device != paNoDevice){
                            dataOutputJs = utils::EmptyTypedArray(env,value->frameCount * value->streamInfo->outputParams.channelCount,value->streamInfo->outputParams.sampleFormat);
                        }

                        auto jsResult = jsCallback.Call({dataInputJs,dataOutputJs, Napi::Number::New(env,value->frameCount), Napi::Number::New(env,value->timeInfo->currentTime)}).ToNumber();
                        
                        if(dataOutputJs.IsTypedArray()){
                            switch (value->streamInfo
                            ->outputParams.sampleFormat)
                            {
                            case paFloat32:
                            {
                                utils::CopyFromTypedArray(dataOutputJs.As<Napi::TypedArrayOf<float>>(),(float *)value->dataOut);
                            }
                            break;

                            case paInt32:
                            {
                                utils::CopyFromTypedArray(dataOutputJs.As<Napi::TypedArrayOf<int32_t>>(),(int32_t *)value->dataOut);
                            }
                            break;

                            case paInt24:
                                break;

                            case paInt16:
                            {
                                utils::CopyFromTypedArray(dataOutputJs.As<Napi::TypedArrayOf<int16_t>>(),(int16_t*)value->dataOut);
                            }
                            break;

                            case paInt8:
                            {
                               utils::CopyFromTypedArray(dataOutputJs.As<Napi::TypedArrayOf<int8_t>>(),(int8_t *)value->dataOut);
                                }
                            break;

                            case paUInt8:
                            {
                                utils::CopyFromTypedArray(dataOutputJs.As<Napi::TypedArrayOf<uint8_t>>(),(uint8_t *)value->dataOut);
                            }
                            break;

                            default:
                                break;
                            }
                        }
                        promise.set_value(jsResult);

                        delete value;
                    });
                    
                    auto returnCode = future.get();
                    
                    if(returnCode != 1){
                        owner->streamInfo->bHasBeenAquired = false;
                        owner->streamInfo->tsfn.Release();
                    }

                    return returnCode; },

                this);

            if (openResult != paNoError)
            {
                delete streamInfo;
                throw Napi::Error::New(env, Pa_GetErrorText(openResult));
            }
        }
        catch (const std::exception &e)
        {
            throw Napi::Error::New(env, e.what());
        }
    }

    void Stream::cleanup()
    {
        if (streamInfo != NULL)
        {

            if (streamInfo->bHasBeenAquired)
            {
                streamInfo->tsfn.Release();
            }

            delete streamInfo;

            streamInfo = NULL;
        }
    }
}
