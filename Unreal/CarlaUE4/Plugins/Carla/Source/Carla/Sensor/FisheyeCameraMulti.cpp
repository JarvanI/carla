// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma de Barcelona (UAB). This work is licensed under the terms of the MIT license. For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"
#include "Carla/Game/CarlaStatics.h"
#include "Components/DrawFrustumComponent.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2DMulti.h"
#include "Carla/Sensor/FisheyeCameraMulti.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ContentStreaming.h"
#include "ImageUtils.h"
#include "ModuleManager.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"
#include "FileHelper.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ParallelFor.h"

uint32 AFisheyeCameraMulti::IDGenerator = 100u;

// =============================================================================
// -- AFisheyeCameraMulti ------------------------------------------------------
// =============================================================================


AFisheyeCameraMulti::AFisheyeCameraMulti(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer)
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;

    PlaneArray.Add(FPlane(1, 0, 0, 1));
    PlaneArray.Add(FPlane(0, 1, 0, -1));
    PlaneArray.Add(FPlane(0, 1, 0, 1));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    Radius = float(ImageWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraMulti::BeginPlay() {
    // SetUpSceneCaptureComponent是个虚函数 , 在ASceneCaptureSensor中里面什么也没有
    // 只有ASceneCaptureSensor的子类 , 比如AShaderBasedSensor , 才会重写SetUpSceneCaptureComponent函数 , 
    // 然后在这里执行 . 是为了调用PostProcessSettings
    // Call derived classes to set up their things.
    //SetUpSceneCaptureComponent(*CaptureComponent2D);

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    CalPixelsRelationship();
    Super::BeginPlay();
}

void AFisheyeCameraMulti::Tick(float DeltaTime) {
    Super::Tick(DeltaTime);
    // Add the view information every tick. Its only used for one tick and then
    // removed by the streamer.
    //IStreamingManager::Get().AddViewInformation(
    //	GetActorLocation(),
    //	GetImageWidth(),
    //	GetImageHeight() / FMath::Tan(GetFOVAngle()));
    //	GetPosInRendertarget().Min.X , GetPosInRendertarget().Min.Y ,
    //	GetPosInRendertarget().Max.X ,GetPosInRendertarget().Max.Y);
    //FPixelReader::SendSplitPixelsInRenderThread(*this, GetPosInRendertarget());

    //test for save image on disk
    //{
    //    FDateTime Time = FDateTime::Now();
    //    int64 Timestamp = Time.ToUnixTimestamp();
    //    FString TimestampStr = FString::FromInt(Timestamp);
    //    FString SaveFileName = FPaths::ProjectSavedDir();
    //    SaveFileName.Append(FString("FishEyeMulti"));
    //    SaveFileName.Append(TimestampStr);
    //    SaveFileName.Append(".jpg");
    //    ScreenshotToImage2D(SaveFileName);
    //}
    SendFisheyeMultiPixelsInRenderThread(*this);
};

void AFisheyeCameraMulti::EndPlay(const EEndPlayReason::Type EndPlayReason) {
    Super::EndPlay(EndPlayReason);
    //CaptureComponent2DMulti->Deactivate();
    for (int i = 0; i < 5; i++)
    {
        CaptureComponent2DMulti->CameraAttributeMap.FindAndRemoveChecked(--IDGenerator);
    }
};

FActorDefinition AFisheyeCameraMulti::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
        TEXT("fisheyemulti"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCameraMulti::Set(const FActorDescription &Description)
{
    Super::Set(Description);
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
    for (int i = 0; i < 5; i++)
    {
        FString name_fstring = FString("FisheyeMultiSceneComponent_") + FString::FromInt(i);
        FName name = FName(*name_fstring);
        CameraSceneComponent[i] = NewObject<USceneComponent>(this, name);
        CameraSceneComponent[i]->RegisterComponent();
        CameraSceneComponent[i]->AttachToComponent(this->RootComponent, FAttachmentTransformRules::KeepRelativeTransform);

        FRotator Rot;
        switch (i)
        {
        case 0:
            Rot = FRotator(0, 0, 0);
            break;
        case 1:
            Rot = FRotator(0, -90, 0);
            break;
        case 2:
            Rot = FRotator(0, 90, 0);
            break;
        case 3:
            Rot = FRotator(90, 0, 0);
            break;
        case 4:
            Rot = FRotator(-90, 0, 0);
            break;
        }
        CameraSceneComponent[i]->SetRelativeRotation(Rot);

        FCameraAttribute CameraAttr;
        CameraAttr.SceneComponent = CameraSceneComponent[i];
        CameraAttr.FOVAngle = 90.0f;
        CameraAttr.OrthoWidth = 512;
        CameraAttr.ProjectionType = ECameraProjectionMode::Perspective;
        CameraAttr.bUseCustomProjectionMatrix = false;
        CameraAttr.CustomProjectionMatrix.SetIdentity();
        CameraAttr.ClipPlaneNormal = FVector(0, 0, 1);
        CameraAttr.bOverride_CustomNearClippingPlane = false;
        CameraAttr.SizeX = ImageWidth;
        CameraAttr.SizeY = ImageWidth;
        CaptureComponent2DMulti->AddCamera(IDGenerator++, CameraAttr);
    }
}

void AFisheyeCameraMulti::SetImageSize(int Width)
{
    ImageWidth = Width;
    Radius = float(ImageWidth) / 2;
}

void AFisheyeCameraMulti::SetSSAA(int Num)
{
    SampleNum = Num;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraMulti::SetProjectionModel(int Model)
{
    ProjectionModel = Model;
}

void AFisheyeCameraMulti::ScreenshotToImage2D(const FString& InImagePath)
{

    if (CaptureComponent2DMulti && CaptureComponent2DMulti->TextureTarget)
    {
        FTextureRenderTargetResource* TextureRenderTargetResource = CaptureComponent2DMulti->TextureTarget->GameThread_GetRenderTargetResource();
        int32 Width = CaptureComponent2DMulti->TextureTarget->SizeX;
        int32 Height = CaptureComponent2DMulti->TextureTarget->SizeY;
        //FIntRect Pos = CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget;
        //uint32 Width = Pos.Max.X - Pos.Min.X;
        //uint32 Height = Pos.Max.Y - Pos.Min.Y;
        if (Width > 0 && Height > 0) {
            TArray<FColor> OutData;
            TextureRenderTargetResource->ReadPixels(OutData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
            //UE_LOG(LogTemp, Warning, TEXT("ID is %d , pos in render target is %d "), OutData.Num(), OutData.GetAllocatedSize()); 
            //CaptureComponent2DMulti->CameraAttributeMap[ID].PosInRenderTarget
            //UE_LOG(LogTemp, Warning, TEXT("after readpixels , TArray size is %d , data size is %d "), OutData.Num(), OutData.GetAllocatedSize());
            ColorToImage(InImagePath, OutData, Width, Height);
            //UE_LOG(LogTemp, Warning, TEXT("width = %d , height = %d , TArray size is %d"), Width, Height, OutData.Num());
        }
    }
    else {
        UE_LOG(LogTemp, Warning, TEXT("NO AFisheyeCameraMulti CaptureComponent2DMulti->TextureTarget"));
    }
}

void AFisheyeCameraMulti::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
{
    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked <IImageWrapperModule>("ImageWrapper");
    FString Ex = FPaths::GetExtension(InImagePath);
    if (Ex.Equals(TEXT("jpg"), ESearchCase::IgnoreCase) || Ex.Equals(TEXT("jpeg"), ESearchCase::IgnoreCase))
    {
        TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
        if (ImageWrapper->SetRaw(InColor.GetData(), InColor.GetAllocatedSize(), InWidth, InHeight, ERGBFormat::BGRA, 8))
        {
            FFileHelper::SaveArrayToFile(ImageWrapper->GetCompressed(100), *InImagePath);
        }
    }
    else
    {
        TArray<uint8> OutPNG;
        for (FColor& color : InColor)
        {
            color.A = 255;
        }
        FImageUtils::CompressImageArray(InWidth, InHeight, InColor, OutPNG);
        FFileHelper::SaveArrayToFile(OutPNG, *InImagePath);
    }
}

void AFisheyeCameraMulti::GetFishEyePic(const FString& InImagePath)
{
    //auto start = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
    //check(FishEyeTexture != nullptr);

    //OutDataFishEye.Init(FColor(0, 0, 0), ImageWidth*ImageWidth);
    //int32 Width = CaptureComponent2DMulti->TextureTarget->SizeX;
    //int32 Height = CaptureComponent2DMulti->TextureTarget->SizeY;
    //FTextureRenderTargetResource* TextureRenderTargetResourceFront = CaptureComponent2D[0]->TextureTarget->GameThread_GetRenderTargetResource();
    //FTextureRenderTargetResource* TextureRenderTargetResourceLeft = CaptureComponent2D[1]->TextureTarget->GameThread_GetRenderTargetResource();
    //FTextureRenderTargetResource* TextureRenderTargetResourceRight = CaptureComponent2D[2]->TextureTarget->GameThread_GetRenderTargetResource();
    //FTextureRenderTargetResource* TextureRenderTargetResourceTop = CaptureComponent2D[3]->TextureTarget->GameThread_GetRenderTargetResource();
    //FTextureRenderTargetResource* TextureRenderTargetResourceBottom = CaptureComponent2D[4]->TextureTarget->GameThread_GetRenderTargetResource();

    //TextureRenderTargetResourceFront->ReadPixels(OutDataFront, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    //TextureRenderTargetResourceLeft->ReadPixels(OutDataLeft, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    //TextureRenderTargetResourceRight->ReadPixels(OutDataRight, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    //TextureRenderTargetResourceTop->ReadPixels(OutDataTop, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
    //TextureRenderTargetResourceBottom->ReadPixels(OutDataBottom, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));

    //auto mid = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
  /*  ParallelFor(ImaginPixelsQuerry.Num(), [&](int i)
    {
        auto& ImagingPixel = ImagingPixels[ImaginPixelsQuerry[i]];
        TArray<FColor> PixelColor;
        TArray<FColor> *OriginPtr;
        for (int k = 0; k < SampleNum; k++)
        {
            for (int l = 0; l < SampleNum; l++)
            {
                int SampleID = k * SampleNum + l;
                if (ImagingPixel.SampleInImage[SampleID] == 0)
                {
                    PixelColor.Add(FColor(0, 0, 0));
                }
                else
                {
                    auto& SampleOrigin = ImagingPixel.SampleInfosArray[SampleID].ISampleOriginPanelImageSpace;
                    TArray<FColor> SampleColor;
                    for (int m = 0; m < SampleOrigin.Num(); m++)
                    {
                        int x = SampleOrigin[m].TexelPos.i;
                        int y = SampleOrigin[m].TexelPos.j;
                        OriginPtr = GetOutData(SampleOrigin[m].ID);
                        SampleColor.Add((*OriginPtr)[x * ImageWidth + y]);
                    }
                    PixelColor.Add(CalAvgColor(SampleColor));
                }
            }
        }
        OutDataFishEye[ImagingPixel.i * ImageWidth + ImagingPixel.j] = CalAvgColor(PixelColor);
    });
    auto end = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();*/

    //UE_LOG(LogTemp, Warning, TEXT("mid -start : %f, end-mid %f"), mid - start, end - mid);

    //ColorToImage(InImagePath, OutDataFishEye, Width, Height);
}

FColor AFisheyeCameraMulti::CalAvgColor(TArray<FColor>& PixelColor)
{
    int R = 0;
    int G = 0;
    int B = 0;
    int A = 0;
    for (int i = 0; i < PixelColor.Num(); i++)
    {
        R += PixelColor[i].R;
        G += PixelColor[i].G;
        B += PixelColor[i].B;
        A += PixelColor[i].A;
    }
    R /= PixelColor.Num();
    G /= PixelColor.Num();
    B /= PixelColor.Num();
    A /= PixelColor.Num();

    return FColor(uint8(R), uint8(G), uint8(B), uint8(A));
}

TArray<FColor>* AFisheyeCameraMulti::GetOutData(int CameraID)
{
    switch (CameraID)
    {
    case 0:
        return &OutDataFront;
        break;
    case 1:
        return &OutDataLeft;
        break;
    case 2:
        return &OutDataRight;
        break;
    case 3:
        return &OutDataTop;
        break;
    case 4:
        return &OutDataBottom;
        break;
    }
    return &OutDataFront;
}


void AFisheyeCameraMulti::CalPixelsRelationship()
{
    //O为球心也是3D局部坐标系的原点 , O在原成像面的投影是o , 相距的距离为单位距离1
    //先假设是stereographic投影 , r = 2ftan(θ/2), 暂时f=1
    //这里要注意 , 坐标系是ue4的左手系 , 红色轴x绿色轴y蓝色轴z . 
    //垂直于成像面朝前是x轴 , 成像面水平方向从左到右为y轴, 从下到上为z轴 , 成像面为yz平面
    //设入射光线从点P经过O , 最后落在成像面上的点p(注意大P小p)
    FVector o(-1, 0, 0);
    FVector O(0, 0, 0);
    FVector oO = O - o;
    FVector YNormal(0, 1, 0);

    int testIsPixelInCircle = 0;
    int testaddpixelpoint = 0;
    //从上到下i, 从左到右j
    for (int i = 0; i < ImageWidth; i++)
    {
        for (int j = 0; j < ImageWidth; j++)
        {
            //sample point
            float Samplei;
            float Samplej;
            int SampleID;
            //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
            for (int k = 0; k < SampleNum; k++)
            {
                for (int l = 0; l < SampleNum; l++)
                {
                    Samplei = float(i) + SampleDist * (2 * k + 1);
                    Samplej = float(j) + SampleDist * (2 * l + 1);

                    if (IsSampleInCircle(Samplei, Samplej))
                    {

                        ImagingPixel* CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
                        if (!CurPixelPtr)
                        {
                            ImaginPixelsQuerry.Add(i * ImageWidth + j);
                            ImagingPixels.Add(i * ImageWidth + j, ImagingPixel(i, j, SampleNum));
                            CurPixelPtr = ImagingPixels.Find(i * ImageWidth + j);
                        }
                        SampleID = k * SampleNum + l;
                        CurPixelPtr->SampleInImage[SampleID] = 1;
                        //UE_LOG(LogTemp, Warning, TEXT("Sample info : No.%d (%lf,%lf) , in circle %d"), SampleID, Samplei, Samplej, CurPixelPtr->SampleInImage[SampleID]);
                        //FVector p(-1, (float(j) + 0.5 - Radius) / Radius, (-(float(i) + 0.5) + Radius) / Radius);
                        FVector p(-1, (Samplej - Radius) / Radius, (-Samplei + Radius) / Radius);
                        FVector po = o - p;
                        FVector pO = O - p;
                        //thetad是pO和oO的夹角 , 也就是逆向的出射光线和x轴正向的夹角
                        float thetad = FMath::Acos(FVector::DotProduct(oO, pO) / (oO.Size() * pO.Size()));
                        //theta是OP和oO轴的夹角 , 也就是逆向的入射光线和x轴正向的夹角
                        float theta;
                        switch (ProjectionModel)
                        {
                        case 0:
                            //透视投影 (perspective projection)
                            theta = thetad;
                            break;
                        case 1:
                            //体视投影 (stereographic projection)
                            theta = 2 * FMath::Atan(FMath::Tan(thetad) / 2);
                            break;
                        case 2:
                            //等距投影 (equidistance projection)
                            theta = FMath::Tan(thetad);
                            break;
                        case 3:
                            //等积投影(equisolid angle projection)
                            theta = 2 * FMath::Asin(FMath::Tan(thetad) / 2);
                            break;
                        case 4:
                            //正交投影 (orthogonal projection)
                            theta = FMath::Asin(FMath::Tan(thetad));
                            break;
                        default:
                            theta = thetad;
                        }

                        //alpha是po和和y轴正向的夹角 , 同样是Op'(p'是P点在yz平面上的投影)和y轴正向的夹角
                        FVector ppie(0, p.Y, p.Z);
                        float alpha = FMath::Acos(FVector::DotProduct(ppie, YNormal) / (ppie.Size() * YNormal.Size()));
                        //上面用反余弦函数求到的角度范围为[0,pi] , 而半球在xy平面的投影(即成像面)的角度是[0,2pi] , 所以需要纠正
                        if (ppie.Z < 0)
                        {
                            alpha = 2 * PI - alpha;
                        }
                        //现在 , 已知OP和x轴正向角度为theta  , Op'和y轴正向的角度为alpha , 计算出OP的单位向量
                        FVector OPNormal(FMath::Cos(theta), FMath::Sin(theta) * FMath::Cos(alpha), FMath::Sin(theta) * FMath::Sin(alpha));
                        //UE_LOG(LogTemp, Warning, TEXT("PixelInfo : (%d,%d)"), i, j);
                        //UE_LOG(LogTemp, Warning, TEXT("p : (%lf,%lf,%lf)"), p.X, p.Y,p.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("po : (%lf,%lf,%lf)"), po.X, po.Y, po.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("pO : (%lf,%lf,%lf)"), pO.X, pO.Y, pO.Z);
                        //UE_LOG(LogTemp, Warning, TEXT("thetad, theta, alpha : (%lf,%lf,%lf)"), thetad, theta, alpha);
                        //UE_LOG(LogTemp, Warning, TEXT("OPNormal : (%lf,%lf,%lf)"), OPNormal.X, OPNormal.Y, OPNormal.Z);

                        for (int m = 0; m < PlaneArray.Num(); m++)
                        {
                            //归一化的空间坐标下的交点
                            bool WillIntersect = false;
                            FVector IntersectPointNormal = RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m], WillIntersect);
                            //FVector IntersectPointNormal = FMath::RayPlaneIntersection(FVector::ZeroVector, OPNormal, PlaneArray[m]);

                            //当找到OP和2D图像的交点
                            if (WillIntersect && IsPointInCube(IntersectPointNormal))
                            {
                                //sample origin和5个panel构成的包围盒只能有1个交点 , 但是这个交点却可能在panel的拼接缝上 , 因此最多可能有3个panel space坐标
                                //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                CurPixelPtr->SampleInfosArray[SampleID].FSampleOriginPanelLocalSpaceNormal = IntersectPointNormal;

                                //局部空间坐标
                                FVector IntersectPoint = IntersectPointNormal * Radius;
                                //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                //连续的屏幕坐标 , 坐标原点在左上角 , 竖直朝下是i(x), 水平朝右是j(y)
                                FVector2D IncidentRayOrigin = LoclSpace2Panel(m, IntersectPoint);

                                if (int(IncidentRayOrigin.X) >= ImageWidth)
                                {
                                    IncidentRayOrigin.X = float(ImageWidth - 1) + 0.5;
                                    UE_LOG(LogTemp, Warning, TEXT("Pixel (%d,%d)"), CurPixelPtr->i, CurPixelPtr->j);
                                    UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                }
                                if (int(IncidentRayOrigin.Y) >= ImageWidth)
                                {
                                    IncidentRayOrigin.Y = float(ImageWidth - 1) + 0.5;
                                }
                                CurPixelPtr->SampleInfosArray[SampleID].ISampleOriginPanelImageSpace.Add(
                                    ITexel(m, int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y)));

                                //UE_LOG(LogTemp, Warning, TEXT("IntersectPoint : (%lf,%lf,%lf)"), IntersectPoint.X, IntersectPoint.Y, IntersectPoint.Z);
                                //UE_LOG(LogTemp, Warning, TEXT("IntersectRayorigin : (%d,%d)"), int(IncidentRayOrigin.X), int(IncidentRayOrigin.Y));
                                //UE_LOG(LogTemp, Warning, TEXT("texel panel id : %d , (%d,%d)"), 
                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].ID, 
                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.i, 
                                //    CurPixelPtr->ISampleOriginTexelImageSpace[SampleID].TexelPos.j);
                                testaddpixelpoint++;
                            }
                        }
                    }
                }
            }
        }
    }
    UE_LOG(LogTemp, Warning, TEXT("testaddpixelpoint %d "), testaddpixelpoint);

    UE_LOG(LogTemp, Warning, TEXT("testIsPixelInCircle %d in the circle"), testIsPixelInCircle);

    UE_LOG(LogTemp, Warning, TEXT("TMap ImagingPixels length :  %d "), ImagingPixels.Num());
}

bool AFisheyeCameraMulti::IsPixelInCircle(int PixelI, int PixelJ)
{
    float PixelCenterI = float(PixelI) + 0.5;
    float PixelCenterJ = float(PixelJ) + 0.5;
    FVector2D PixelCenter(PixelCenterI, PixelCenterJ);

    FVector2D ImageCenter(Radius, Radius);
    float Dist = (ImageCenter - PixelCenter).Size();
    //UE_LOG(LogTemp, Warning, TEXT("PixelCenter : (%lf,%lf),  Dist : %lf "), PixelCenter.X,PixelCenter.Y,Dist);    //1740388
    if (Dist <= Radius)
    {
        return true;
    }
    else
    {
        return false;
    }
}

FVector2D AFisheyeCameraMulti::LoclSpace2Panel(int PanelID, FVector IntersectPoint)
{
    FVector2D IncidentRayOrigin;
    switch (PanelID)
    {
        //front , x = 1
    case 0:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, IntersectPoint.Y + Radius);
        break;
        //left, y = -1
    case 1:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, IntersectPoint.X + Radius);
        break;
        //right, y = 1
    case 2:
        IncidentRayOrigin = FVector2D(-IntersectPoint.Z + Radius, -IntersectPoint.X + Radius);
        break;
        //top, z = 1
    case 3:
        IncidentRayOrigin = FVector2D(IntersectPoint.X + Radius, IntersectPoint.Y + Radius);
        break;
        //bottom, z = -1
    case 4:
        IncidentRayOrigin = FVector2D(-IntersectPoint.X + Radius, IntersectPoint.Y + Radius);
        break;
    }
    return IncidentRayOrigin;
}


bool AFisheyeCameraMulti::IsSampleInCircle(float i, float j)
{
    FVector2D SamplePoint(i, j);

    FVector2D ImageCenter(Radius, Radius);
    float Dist = (ImageCenter - SamplePoint).Size();
    if (Dist <= Radius)
    {
        return true;
    }
    else
    {
        return false;
    }
}


FVector2D AFisheyeCameraMulti::PixelCoord2ImageCoord(int PixelI, int PixelJ)
{
    //2D的image坐标系以成像面中心为原点 , 从左到右为y轴, 从下到上为z轴
    return FVector2D((float(PixelJ) + 0.5 - Radius) / Radius, -(float(PixelI) + 0.5) + Radius) / Radius;
}


//FMath::RayPlaneIntersection几个特点 :
//系数Distance可正可负 . 这就说明实际上是判断直线line和平面交点而不是射线ray和平面交点
//如果线面平行, 返回的FVector是(-nan(ind),-nan(ind),inf) , 所以要自己判断是否平行
//这个函数仅在Distance不为0和无穷大的时候才返回true
FVector AFisheyeCameraMulti::RayPlaneIntersection(const FVector& RayOrigin, const FVector& RayDirection, const FPlane& Plane, bool& WillIntersection)
{

    const FVector PlaneNormal = FVector(Plane.X, Plane.Y, Plane.Z);
    //w也可以是法线与平面交点到原点距离
    const FVector PlaneOrigin = PlaneNormal * Plane.W;

    const float Distance = FVector::DotProduct((PlaneOrigin - RayOrigin), PlaneNormal) / FVector::DotProduct(RayDirection, PlaneNormal);
    if (Distance >= 0.0)
    {
        WillIntersection = true;
    }
    else
    {
        WillIntersection = false;
    }

    return RayOrigin + RayDirection * Distance;
}

bool AFisheyeCameraMulti::IsPointInCube(FVector Point)
{
    if ((Point.X >= 0.0) && (Point.X <= 1.0)
        && (Point.Y >= -1.0) && (Point.Y <= 1.0)
        && (Point.Z >= -1.0) && (Point.Z <= 1.0))
    {
        return true;
    }
    return false;
}


void AFisheyeCameraMulti::SendFisheyeMultiPixelsInRenderThread(AFisheyeCameraMulti &Sensor)
{
    check(CaptureRenderTarget != nullptr);
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteFisheyeMultiPixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = this->GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();
            //carla::Buffer *Buffers[5];
            //for (int i = 0; i < 5; i++)
            //{
            //    Buffers[i] = Stream.PopBufferFromPool();
            //}

            auto t2 = std::chrono::system_clock::now();

            Sensor.WriteFisheyeMultiPixelsToBuffer(
                Buffer,
                carla::sensor::SensorRegistry::get<AFisheyeCameraMulti *>::type::header_offset,
                Sensor,
                InRHICmdList);

            auto t3 = std::chrono::system_clock::now();

            Stream.Send(Sensor, std::move(Buffer));

            auto t4 = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
            std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
            std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
            //WritePixelsToBuffer��ʱ5ms
            UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void AFisheyeCameraMulti::WriteFisheyeMultiPixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    AFisheyeCameraMulti &Sensor,
    FRHICommandListImmediate &
#if CARLA_WITH_VULKAN_SUPPORT == 1
    InRHICmdList
#endif // CARLA_WITH_VULKAN_SUPPORT
)
{
    check(IsInRenderingThread());
    auto t1 = std::chrono::system_clock::now();

    FRHITexture2D *Texture;
    const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
    uint32 MultiWidth;
    uint32 MultiHeight;
    uint32 ExpectedStride;
    uint32 SrcStride;
    uint8 *Source;

    Texture = CaptureRenderTarget->GetRenderTargetResource()->GetRenderTargetTexture();
    checkf(Texture != nullptr, TEXT("FPixelReader: FisheyeCameraMulti UTextureRenderTarget2D missing render target texture"));
    MultiWidth = Texture->GetSizeX();
    MultiHeight = Texture->GetSizeY();
    ExpectedStride = MultiWidth * BytesPerPixel;
    Source = (reinterpret_cast<uint8*>(RHILockTexture2D(Texture, 0, RLM_ReadOnly, SrcStride, false)));

    UE_LOG(LogTemp, Warning, TEXT("Jarvan AFisheyeCameraMulti,Width:%d,Height:%d,ExpectedStride:%d,SrcStride:%d"), 
        MultiWidth, MultiHeight, ExpectedStride, SrcStride);
    auto t2 = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;

    int MultiExpectedStride = MultiWidth + (SrcStride - ExpectedStride) / 4;
#ifdef PLATFORM_WINDOWS
    // JB: Direct 3D uses additional rows in the buffer, so we need check the
    // result stride from the lock:
    //d3d平台单独处理是因为每一行末尾会有额外的数据 , 而其他api没有 , 所以其他api可以整块直接复制
    //经过测试, 每行末尾比原数据多了32字节
    if (IsD3DPlatform(GMaxRHIShaderPlatform, false) && (ExpectedStride != SrcStride))
    {
        MultiExpectedStride = MultiWidth + (SrcStride - ExpectedStride)/4;
    }
#endif // PLATFORM_WINDOWS

    Buffer.reset(Offset + ImageWidth * ImageWidth * 4);
    auto DstRow = Buffer.begin() + Offset;
    ParallelFor(ImaginPixelsQuerry.Num(), [&](int i)
    {
        auto& ImagingPixel = ImagingPixels[ImaginPixelsQuerry[i]];
        uint32 R = 0;
        uint32 G = 0;
        uint32 B = 0;
        uint32 A = 0;
        for (int k = 0; k < SampleNum; k++)
        {
            for (int l = 0; l < SampleNum; l++)
            {
                int SampleID = k * SampleNum + l;
                if (ImagingPixel.SampleInImage[SampleID] == 0)
                {
                    //xelColor.Add(FColor(0, 0, 0));
                }
                else
                {
                    uint32 RS = 0;
                    uint32 GS = 0;
                    uint32 BS = 0;
                    uint32 AS = 0;
                    auto& SampleOrigin = ImagingPixel.SampleInfosArray[SampleID].ISampleOriginPanelImageSpace;
                    for (int m = 0; m < SampleOrigin.Num(); m++)
                    {
                        int x = SampleOrigin[m].TexelPos.i;
                        int y = SampleOrigin[m].TexelPos.j;
                        auto CameraAttrPtr = CaptureComponent2DMulti->CameraAttributeMap.Find(100 + SampleOrigin[m].ID);
                        if(CameraAttrPtr)
                        {
                            x = x + CameraAttrPtr->PosInRenderTarget.Min.Y;
                            y = y + CameraAttrPtr->PosInRenderTarget.Min.X;
                            if (Source)
                            {
                                RS += *(Source + (x * MultiExpectedStride + y) * 4u);
                                GS += *(Source + (x * MultiExpectedStride + y) * 4u + 1);
                                BS += *(Source + (x * MultiExpectedStride + y) * 4u + 2);
                                AS += *(Source + (x * MultiExpectedStride + y) * 4u + 3);
                            }
                        }
                    }
                    R += (RS / SampleOrigin.Num());
                    G += (GS / SampleOrigin.Num());
                    B += (BS / SampleOrigin.Num());
                    A += (AS / SampleOrigin.Num());
                    //PixelColor.Add(CalAvgColor(SampleColor));
                }
            }
        }
        uint8 Color[4];
        Color[0] = R / (SampleNum * SampleNum);
        Color[1] = G / (SampleNum * SampleNum);
        Color[2] = B / (SampleNum * SampleNum);
        Color[3] = A / (SampleNum * SampleNum);
        //FColor ResultColor = CalAvgColor(PixelColor);
        //FMemory::Memcpy(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u , &Color, 4);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u) = R / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 1) = G / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 2) = B / (SampleNum * SampleNum);
        *(DstRow + (ImagingPixel.i * ImageWidth + ImagingPixel.j) * 4u + 3) = A / (SampleNum * SampleNum);
    });
    auto t3 = std::chrono::system_clock::now();

    std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
    //  LockTexture Lock(Texture, SrcStride);耗时3ms
    UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time AFisheyeCamera::WriteFisheyePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());


    //TArray<uint8> Color(DstRow + Offset, ImageWidth * ImageWidth * 4);
    //FDateTime Time = FDateTime::Now();
    //int64 Timestamp = Time.ToUnixTimestamp();
    //FString TimestampStr = FString::FromInt(Timestamp);
    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("FishEye"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    //ColorToImage(SaveFileName, Color, ImageWidth * 4, ImageWidth);

    RHIUnlockTexture2D(Texture, 0, false);
}



























void AFisheyeCameraMulti::SetExposureMethod(EAutoExposureMethod Method)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod = Method;
}

EAutoExposureMethod AFisheyeCameraMulti::GetExposureMethod() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCameraMulti::SetExposureCompensation(float Compensation)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias = Compensation;
}

float AFisheyeCameraMulti::GetExposureCompensation() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCameraMulti::SetShutterSpeed(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed = Speed;
}

float AFisheyeCameraMulti::GetShutterSpeed() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCameraMulti::SetISO(float ISO)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.CameraISO = ISO;
}

float AFisheyeCameraMulti::GetISO() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.CameraISO;
}

void AFisheyeCameraMulti::SetAperture(float Aperture)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop = Aperture;
}

float AFisheyeCameraMulti::GetAperture() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCameraMulti::SetFocalDistance(float Distance)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
}

float AFisheyeCameraMulti::GetFocalDistance() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCameraMulti::SetDepthBlurAmount(float Amount)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
}

float AFisheyeCameraMulti::GetDepthBlurAmount() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCameraMulti::SetDepthBlurRadius(float Radius)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
}

float AFisheyeCameraMulti::GetDepthBlurRadius() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCameraMulti::SetDepthOfFieldMinFstop(float MinFstop)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
}

float AFisheyeCameraMulti::GetDepthOfFieldMinFstop() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCameraMulti::SetBladeCount(int Count)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount = Count;
}

int AFisheyeCameraMulti::GetBladeCount() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCameraMulti::SetFilmSlope(float Slope)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmSlope = Slope;
}

float AFisheyeCameraMulti::GetFilmSlope() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmSlope;
}

void AFisheyeCameraMulti::SetFilmToe(float Toe)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
}

float AFisheyeCameraMulti::GetFilmToe() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmToe;
}

void AFisheyeCameraMulti::SetFilmShoulder(float Shoulder)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmShoulder = Shoulder;
}

float AFisheyeCameraMulti::GetFilmShoulder() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmShoulder;
}

void AFisheyeCameraMulti::SetFilmBlackClip(float BlackClip)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip = BlackClip;
}

float AFisheyeCameraMulti::GetFilmBlackClip() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCameraMulti::SetFilmWhiteClip(float WhiteClip)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip = WhiteClip;
}

float AFisheyeCameraMulti::GetFilmWhiteClip() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCameraMulti::SetExposureMinBrightness(float Brightness)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness = Brightness;
}

float AFisheyeCameraMulti::GetExposureMinBrightness() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCameraMulti::SetExposureMaxBrightness(float Brightness)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
}

float AFisheyeCameraMulti::GetExposureMaxBrightness() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCameraMulti::SetExposureSpeedDown(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown = Speed;
}

float AFisheyeCameraMulti::GetExposureSpeedDown() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCameraMulti::SetExposureSpeedUp(float Speed)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp = Speed;
}

float AFisheyeCameraMulti::GetExposureSpeedUp() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCameraMulti::SetExposureCalibrationConstant(float Constant)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
}

float AFisheyeCameraMulti::GetExposureCalibrationConstant() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.AutoExposureCalibrationConstant;
}

void AFisheyeCameraMulti::SetMotionBlurIntensity(float Intensity)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount = Intensity;
}

float AFisheyeCameraMulti::GetMotionBlurIntensity() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCameraMulti::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax = MaxDistortion;
}

float AFisheyeCameraMulti::GetMotionBlurMaxDistortion() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCameraMulti::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
}

float AFisheyeCameraMulti::GetMotionBlurMinObjectScreenSize() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCameraMulti::SetWhiteTemp(float Temp)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.WhiteTemp = Temp;
}

float AFisheyeCameraMulti::GetWhiteTemp() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.WhiteTemp;
}

void AFisheyeCameraMulti::SetWhiteTint(float Tint)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.WhiteTint = Tint;
}

float AFisheyeCameraMulti::GetWhiteTint() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.WhiteTint;
}

void AFisheyeCameraMulti::SetChromAberrIntensity(float Intensity)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity = Intensity;
}

float AFisheyeCameraMulti::GetChromAberrIntensity() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCameraMulti::SetChromAberrOffset(float Offset)
{
    check(CaptureComponent2DMulti != nullptr);
    CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
}

float AFisheyeCameraMulti::GetChromAberrOffset() const
{
    check(CaptureComponent2DMulti != nullptr);
    return CaptureComponent2DMulti->PostProcessSettings.ChromaticAberrationStartOffset;
}