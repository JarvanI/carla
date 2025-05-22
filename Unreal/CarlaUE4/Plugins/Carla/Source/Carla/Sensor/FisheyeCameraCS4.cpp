#include "Carla.h"
#include "FisheyeCameraCS4.h"
#include "FisheyeCS4Camera/Public/FisheyeCS4CameraRendering.h"
#include "Carla/Game/CarlaStatics.h"
#include "Components/DrawFrustumComponent.h"
#include "Engine/Classes/Engine/Scene.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HighResScreenshot.h"
#include "ContentStreaming.h"
#include "FileHelper.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "IImageWrapperModule.h"
#include "ModuleManager.h"
#include "ParallelFor.h"
#include "Actor/ActorBlueprintFunctionLibrary.h"

static float Tolerance = 1e-5f;
static auto FISHEYECS4_COUNTER = 0u;

// =============================================================================
// -- Local static methods -----------------------------------------------------
// =============================================================================

// Local namespace to avoid name collisions on unit builds.
namespace FisheyeCameraCS4_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D);

    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing = true);

    static auto GetQualitySettings(UWorld *World)
    {
        auto Settings = UCarlaStatics::GetCarlaSettings(World);
        check(Settings != nullptr);
        return Settings->GetQualityLevel();
    }
} // namespace FisheyeCameraCS_local_ns

// =============================================================================
// -- AFisheyeCameraCS4 ------------------------------------------------------
// =============================================================================



FVector LocalSpace2Panel(int PanelID, FVector IntersectPoint)
{
    FMatrix TranslationMatrix;
    FMatrix RotationMatrix;
    FMatrix M;
    switch (PanelID)
    {
        //left
    case 0:
        TranslationMatrix = FTranslationMatrix(FVector(0.0f, -UE_SQRT_2, 1.0f));
        //for(int i=0;i<4;i++)
        //{
        //    UE_LOG(LogTemp, Warning, TEXT("TranslationMatrix:(%lf, %lf, %lf,%lf)"),
        //        TranslationMatrix.M[0][i], TranslationMatrix.M[1][i], TranslationMatrix.M[2][i], TranslationMatrix.M[3][i]);
        //}
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(0.0f, 0.0f, -1.0f));
        //for (int i = 0; i < 4; i++)
        //{
        //    UE_LOG(LogTemp, Warning, TEXT("RotationMatrix:(%lf, %lf, %lf,%lf)"),
        //        RotationMatrix.M[0][i], RotationMatrix.M[1][i], RotationMatrix.M[2][i], RotationMatrix.M[3][i]);
        //}
        break;
        //right
    case 1:
        TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0f, 1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(0, 0.0f, -1.0f));
        break;
        //top, y = 1
    case 2:
        TranslationMatrix = FTranslationMatrix(FVector(-UE_SQRT_2, 0.0f, 1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(UE_SQRT_2 / 2, -UE_SQRT_2, 0.0f));
        break;
        //    //bottom, z = 1
    case 3:
        TranslationMatrix = FTranslationMatrix(FVector(UE_SQRT_2, 0.0, -1.0f));
        RotationMatrix = FRotationMatrix::MakeFromXY(
            FVector(-UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.0f),
            FVector(-UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.0f));
        break;
    }
    M = RotationMatrix * TranslationMatrix;
    //for (int i = 0; i < 4; i++)
    //{
    //    UE_LOG(LogTemp, Warning, TEXT("M:(%lf, %lf, %lf,%lf)"),
    //        M.M[0][i], M.M[1][i], M.M[2][i], M.M[3][i]);
    //}
    M = M.Inverse();
    //for (int i = 0; i < 4; i++)
    //{
    //    UE_LOG(LogTemp, Warning, TEXT("M.inverse:(%lf, %lf, %lf,%lf)"),
    //        M.M[0][i], M.M[1][i], M.M[2][i], M.M[3][i]);
    //}
    FVector4 Res = M.TransformPosition(IntersectPoint);
    return FVector(Res.X, Res.Y, Res.Z)/2;
}


// 判断两个线段是否在2D平面内相交
bool DoSegmentsIntersect(const FVector& p1, const FVector& p2, const FVector& q1, const FVector& q2)
{
    auto Cross = [](const FVector2D& a, const FVector2D& b) {
        return a.X * b.Y - a.Y * b.X;
    };

    auto To2D = [](const FVector& v) {
        return FVector2D(v.X, v.Y); // 投影到XY平面
    };

    FVector2D r = To2D(p2 - p1);
    FVector2D s = To2D(q2 - q1);
    FVector2D pq = To2D(q1 - p1);

    float rxs = Cross(r, s);
    float pqxr = Cross(pq, r);

    if (FMath::IsNearlyZero(rxs)) return false; // 平行或共线

    float t = Cross(pq, s) / rxs;
    float u = pqxr / rxs;

    return (t > 0 && t < 1) && (u > 0 && u < 1);
}

// 判断多边形是否是简单多边形（即不自交）
bool IsSimplePolygon(const TArray<FVector>& Points)
{
    int32 Num = Points.Num();
    for (int32 i = 0; i < Num; ++i)
    {
        FVector A1 = Points[i];
        FVector A2 = Points[(i + 1) % Num];

        for (int32 j = i + 1; j < Num; ++j)
        {
            // 跳过共顶点或相邻边
            if ((j == i) || (j == (i + 1) % Num) || ((i == 0 && j == Num - 1))) continue;

            FVector B1 = Points[j];
            FVector B2 = Points[(j + 1) % Num];

            if (DoSegmentsIntersect(A1, A2, B1, B2))
            {
                return false;
            }
        }
    }
    return true;
}





// 计算2D多边形面积，自动检测是否自交
float AFisheyeCameraCS4::ComputePolygonArea2D(const TArray<FVector>& Points)
{
    int32 NumPoints = Points.Num();
    if (NumPoints < 3) return 0.0f;

    if (!IsSimplePolygon(Points))
    {
        return -1.0f; // 标记非法自交
    }

    float Area = 0.0f;
    for (int32 i = 0; i < NumPoints; ++i)
    {
        const FVector& P1 = Points[i];
        const FVector& P2 = Points[(i + 1) % NumPoints];
        Area += (P1.X * P2.Y - P2.X * P1.Y);
    }

    return FMath::Abs(Area) * 0.5f;
}



bool IsIntersectingVertex(FVector& P, TArray<int>& PanelID)
{
    if (P.Equals(FVector(UE_SQRT_2, 0, 1)))
    {
        PanelID.Add(0);
        PanelID.Add(1);
        PanelID.Add(2);
        return true;
    }
    else if (P.Equals(FVector(UE_SQRT_2, 0, -1)))
    {
        PanelID.Add(0);
        PanelID.Add(1);
        PanelID.Add(3);
        return true;
    }
    else
    {
        return false;
    }
}
bool IsIntersectingEdge(FVector& P, TArray<int>& PanelID)
{
    if (IsIntersectingVertex(P, PanelID))
    {
        return true;
    }
    else
    {
        if (P.X - P.Y == UE_SQRT_2 && P.Z == 1)
        {
            PanelID.Add(0);
            PanelID.Add(2);
            return true;
        }
        else if (P.X - P.Y == UE_SQRT_2 && P.Z == -1)
        {
            PanelID.Add(0);
            PanelID.Add(3);
            return true;
        }
        else if (P.X + P.Y == UE_SQRT_2 && P.Z == 1)
        {
            PanelID.Add(1);
            PanelID.Add(2);
            return true;
        }
        else if (P.X + P.Y == UE_SQRT_2 && P.Z == -1)
        {
            PanelID.Add(1);
            PanelID.Add(3);
            return true;

        }
        else if (P.X == UE_SQRT_2 && P.Y == 0.0)
        {
            PanelID.Add(0);
            PanelID.Add(1);
            return true;
        }
    }
    return false;
}

int HasCommonFace(FPointInfo& P1, FPointInfo& P2)
{
    for (int i = 0; i < P1.FaceIndex.Num(); i++)
    {
        for (int j = 0; j < P2.FaceIndex.Num(); j++)
        {
            if (P1.FaceIndex[i] == P2.FaceIndex[j])
            {
                return P1.FaceIndex[i];
            }
        }
    }
    return -1;
}

FPlane NormalizePlane(const FPlane& P)
{
    FVector N = FVector(P.X, P.Y, P.Z);
    float Len = N.Size();
    if (Len <= KINDA_SMALL_NUMBER)
    {
        return FPlane(0, 0, 0, 0); // 防止除以0
    }
    FVector Normalized = N / Len;
    return FPlane(Normalized, P.W / Len); // W 也要除以 Len
}

FVector IntersectThreePlanes(const FPlane& P1, const FPlane& P2, const FPlane& P3)
{

    FPlane NP1 = NormalizePlane(P1);
    FPlane NP2 = NormalizePlane(P2);
    FPlane NP3 = NormalizePlane(P3);

    FVector a = NP1.GetUnsafeNormal();
    FVector b = NP2.GetUnsafeNormal();
    FVector c = NP3.GetUnsafeNormal();
    a.Normalize();
    b.Normalize();
    c.Normalize();

    float d1 = -NP1.W;
    float d2 = -NP2.W;
    float d3 = -NP3.W;

    FVector bxc = FVector::CrossProduct(b, c);
    FVector cxa = FVector::CrossProduct(c, a);
    FVector axb = FVector::CrossProduct(a, b);

    const float denom = FVector::DotProduct(a, bxc);
    if (FMath::IsNearlyZero(denom))
    {
        return FVector::ZeroVector;
    }

    const FVector result = (-d1 * bxc + -d2 * cxa + -d3 * axb) / denom;

    if (result.ContainsNaN() ||
        !FMath::IsFinite(result.X) ||
        !FMath::IsFinite(result.Y) ||
        !FMath::IsFinite(result.Z))
    {
        return FVector::ZeroVector;
    }
    return result;
}

FVector newIntersectThreePlanesMatrix(const FPlane& P1, const FPlane& P2, const FPlane& P3)
{
    // 构建系数矩阵 A（列向量为每个平面的法向量）
    const FVector a = P1.GetUnsafeNormal();
    const FVector b = P2.GetUnsafeNormal();
    const FVector c = P3.GetUnsafeNormal();

    FMatrix A(
        FPlane(a.X, b.X, c.X, 0.0f),
        FPlane(a.Y, b.Y, c.Y, 0.0f),
        FPlane(a.Z, b.Z, c.Z, 0.0f),
        FPlane(0, 0, 0, 1.0f) // 齐次分量，不参与运算
    );

    // 构建常数向量 -D
    FVector D(-P1.W, -P2.W, -P3.W);

    // 解线性方程 Ax = -d ==> x = A⁻¹ * -d
    const float Determinant = A.Determinant();
    if (FMath::IsNearlyZero(Determinant))
    {
        return FVector::ZeroVector; // 平面不共点或两两平行
    }

    // 使用逆矩阵乘以常数向量
    FMatrix InverseA = A.InverseFast(); // 对于3x3逆来说足够精度
    FVector Intersection = InverseA.TransformVector(D);

    if (Intersection.ContainsNaN())
    {
        return FVector::ZeroVector;
    }

    return Intersection;
}


bool SmallerAndEqual(float A, float B)
{
    return FMath::IsNearlyEqual(A, B, 0.00001f) || (A < B);
}

bool IsInRange(FVector Point)
{
    float x = Point.X, y = Point.Y, z = Point.Z;
    if (SmallerAndEqual(-1.0f, z) && SmallerAndEqual(z, 1.0f) && SmallerAndEqual(0.0f, x))
    {
        if (SmallerAndEqual(-UE_SQRT_2, y) && SmallerAndEqual(y, 0.0f))
        {
            if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, y + UE_SQRT_2))
            {
                return true;
            }
        }
        else if (SmallerAndEqual(0.0f, y) && SmallerAndEqual(y, UE_SQRT_2))
        {
            if (SmallerAndEqual(0.0f, x) && SmallerAndEqual(x, -y + UE_SQRT_2))
            {
                return true;
            }
        }
    }

    return false;
}


void SplitPointsByFace(TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups)
{
    TArray<FPlane> PlaneArray;
    PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    //统计所有点所在面
    TArray<int> FaceCount;
    FaceCount.Init(0, 4);
    for (const FPointInfo& P : InputPoints)
    {
        for (int FaceID : P.FaceIndex)
        {
            FaceCount[FaceID]++;
        }
    }
    int FaceID = -1;
    bool IsOnSameFace = false;
    for (int i = 0; i < FaceCount.Num(); i++)
    {
        //如果所有点都有一个共面, 则共面
        if (FaceCount[i] == InputPoints.Num())
        {
            IsOnSameFace = true;
            FaceID = i;
            break;
        }
    }
    //全部点都在同一个面上, 直接顺序返回
    if (IsOnSameFace)
    {
        TArray<FVector> P;
        for (const FPointInfo& Info : InputPoints)
        {
            P.Add(Info.WorldPos);
        }
        OutGroups[FaceID] = P;
    }
    else   //包括了点不共面, 和点在边上或三面点上
    {
        int LastCommonFaceID = -1;
        for (int i = 0; i < InputPoints.Num(); i++)
        {
            //循环
            FPointInfo PStart = InputPoints[i];
            FPointInfo PEnd = InputPoints[(i + 1) % InputPoints.Num()];
            int CommonFaceID = HasCommonFace(PStart, PEnd);
            if (CommonFaceID != -1)
            {
                // 如果换面 ,就要把上一面的末端点加入
                if (LastCommonFaceID != -1 && LastCommonFaceID != CommonFaceID)
                {
                    OutGroups[LastCommonFaceID].Add(PStart.WorldPos);
                }
                OutGroups[CommonFaceID].Add(PStart.WorldPos);
                LastCommonFaceID = CommonFaceID;
            }
            else
            {
                //不同面，考虑插入交点
                //最简单的 , 各自一面 , 检测原点和两个点构成面和两个面形成的交点, 各自添加到group里
                FPlane PlaneA(PStart.WorldPos, PEnd.WorldPos, FVector::ZeroVector);
                TArray<FVector> IntersectPoints;
                for (int i = 0; i < PStart.FaceIndex.Num(); i++)
                {
                    for (int j = 0; j < PEnd.FaceIndex.Num(); j++)
                    {
                        FPlane PlaneB = PlaneArray[i];
                        FPlane PlaneC = PlaneArray[j];
                        IntersectPoints.Add(IntersectThreePlanes(PlaneA, PlaneB, PlaneC));
                    }
                }
                FVector IntersectPoint;
                for (int i = 0; i < IntersectPoints.Num(); i++)
                {
                    if (IsInRange(IntersectPoints[i]))
                    {
                        IntersectPoint = IntersectPoints[i];
                        break;
                    }
                }

                OutGroups[LastCommonFaceID].Add(PStart.WorldPos);
                OutGroups[LastCommonFaceID].Add(IntersectPoint);
                OutGroups[CommonFaceID].Add(IntersectPoint);
                LastCommonFaceID = CommonFaceID;


                //问题是, 当投影到3个面的时候 , 要怎么把新增的三面的顶点加到组里
            }
        }
        //补上最后一个点以闭合面，避免遗漏末尾
        if (LastCommonFaceID != -1)
        {
            OutGroups[LastCommonFaceID].Add(InputPoints.Last().WorldPos);
        }
    }
}



float GetSegmentTProjection(const FVector& A, const FVector& B, const FVector& P)
{
    FVector AB = B - A;
    FVector AP = P - A;

    float LengthSq = AB.SizeSquared();
    if (LengthSq < KINDA_SMALL_NUMBER)
        return 0.0f;

    float T = FVector::DotProduct(AB, AP) / LengthSq;
    return T;
}



bool IsPointOnPlane(const FVector& Point, const FPlane& Plane, float Tolerance = KINDA_SMALL_NUMBER)
{
    return FMath::Abs(Plane.PlaneDot(Point)) <= Tolerance;
}

bool IsSegSame(FSegment seg)
{
    if (seg.PStart.FaceIndex.Num() != seg.PEnd.FaceIndex.Num())
    {
        return false;
    }else
    {
        for(int i = 0;i< seg.PStart.FaceIndex.Num();i++)
        {
            if(seg.PStart.FaceIndex[i] != seg.PEnd.FaceIndex[i])
            {
                return false;
            }
        }
    }
    return true;
}

bool IsSegHasSameFaces(FSegment& seg, TArray<int>& SameFaces)
{
    for (int i = 0; i < seg.PStart.FaceIndex.Num(); i++)
    {
        for (int j = 0; j < seg.PEnd.FaceIndex.Num(); j++)
        {
            if (seg.PStart.FaceIndex[i] == seg.PEnd.FaceIndex[j])
            {
                SameFaces.Add(seg.PStart.FaceIndex[i]);
                break;
            }

        }
    }
    return SameFaces.Num();
}

bool AFisheyeCameraCS4::IsPointOnEdge(FVector Point)
{
    return (FMath::IsNearlyZero(Point.X, Tolerance) ||
            FMath::IsNearlyZero(Point.Y, Tolerance) ||
            FMath::IsNearlyEqual(Point.X, 1.0f, Tolerance) ||
            FMath::IsNearlyEqual(Point.Y, 1.0f, Tolerance));
}

bool AFisheyeCameraCS4::IsOnSameEdge(FVector P1, FVector P2)
{
    return ((FMath::IsNearlyZero(P1.X, Tolerance) && FMath::IsNearlyZero(P2.X, Tolerance))||
        (FMath::IsNearlyZero(P1.Y, Tolerance) && FMath::IsNearlyZero(P2.Y, Tolerance)) ||
        (FMath::IsNearlyEqual(P1.X, 1.0f, Tolerance) && FMath::IsNearlyEqual(P2.X, 1.0f, Tolerance)) ||
        (FMath::IsNearlyEqual(P1.Y, 1.0f, Tolerance) && FMath::IsNearlyEqual(P2.Y, 1.0f, Tolerance)));
}

bool AFisheyeCameraCS4::AddIfCantFind(TArray<FVector>& Group, FVector Point)
{
    bool Found = false;
    for(FVector P : Group)
    {
        if(P.Equals(Point,Tolerance))
        {
            Found = true;
            break;
        }
    }
    if(Found)
    {
        return false;
    }
    Group.Add(Point);
    return true;
}

void AFisheyeCameraCS4::SplitPoints(TArray<FPointInfo>& InputPoints, TArray<TArray<FVector>>& OutGroups)
{
    TArray<FPlane> PlaneArray;
    PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    int CountBefore = InputPoints.Num();
    bool bAddNewPoint = false;

    //生成面之间的边上的分割点
    for (int i = 0; i < CountBefore; i++)
    {
        FSegment seg(InputPoints[i], InputPoints[(i + 1) % CountBefore]);
        int SharedFaceCount = 0;

        for (int j : seg.PStart.FaceIndex)
        {
            for (int k : seg.PEnd.FaceIndex)
            {
                if (j == k)
                {
                    SharedFaceCount++;
                }
            }
        }

        //两个点不共面 , 就要计算边界点
        if (SharedFaceCount == 0)
        {
            FPlane splitPlane(seg.PStart.WorldPos, seg.PEnd.WorldPos, FVector::ZeroVector);
            TArray<FPointInfo> InsertedPoints;
            TArray<float> TList;

            for (int FaceA = 0; FaceA < PlaneArray.Num() - 1; FaceA++)
            {
                for (int FaceB = FaceA + 1; FaceB < PlaneArray.Num(); FaceB++)
                {
                    // ✅ 排除无效索引（即只允许 AB 各自的面组合）
                    if (!(seg.PStart.FaceIndex.Contains(FaceA) || seg.PEnd.FaceIndex.Contains(FaceA) || 
                        seg.PStart.FaceIndex.Contains(FaceB) || seg.PEnd.FaceIndex.Contains(FaceB)))
                        continue;

                    const FPlane& P1 = splitPlane;
                    const FPlane& P2 = PlaneArray[FaceA];
                    const FPlane& P3 = PlaneArray[FaceB];

                    FVector pt = IntersectThreePlanes(P1, P2, P3);
                    if (pt.Equals(FVector::ZeroVector))
                        continue;

                    bool bAlreadyInserted = false;
                    for (const FPointInfo& Existing : InsertedPoints)
                    {
                        if (FVector::DistSquared(Existing.WorldPos, pt) < KINDA_SMALL_NUMBER)
                        {
                            bAlreadyInserted = true;
                            break;
                        }
                    }
                    if (bAlreadyInserted || !IsInRange(pt))
                        continue;

                    float t = GetSegmentTProjection(seg.PStart.WorldPos, seg.PEnd.WorldPos, pt);

                    if (t < Tolerance || t > 1.0f - Tolerance)
                        continue;

                    //if (FMath::IsNearlyEqual(t, 0.0f, Tolerance)) t = 0.0f;
                    //else if (FMath::IsNearlyEqual(t, 1.0f, Tolerance)) t = 1.0f;
                    
                    // ✅ 提取落在哪些面上
                    TArray<int> FaceIDs;
                    for (int n = 0; n < PlaneArray.Num(); ++n)
                    {
                        if (IsPointOnPlane(pt, PlaneArray[n]))
                        {
                            FaceIDs.Add(n);
                        }
                    }

                    FPointInfo NewPt(pt, FaceIDs);
                    bAddNewPoint = true;

                    // ✅ 插入排序按 t 值升序插入
                    bool bInserted = false;
                    for (int p = 0; p < TList.Num(); ++p)
                    {
                        if (t < TList[p])
                        {
                            InsertedPoints.Insert(NewPt, p);
                            TList.Insert(t, p);
                            bInserted = true;
                            break;
                        }
                    }
                    if (!bInserted)
                    {
                        InsertedPoints.Add(NewPt);
                        TList.Add(t);
                    }
                }
            }


            // 插入交点到 InputPoints（从后往前插入防止索引错乱）
            for (int q = InsertedPoints.Num() - 1; q >= 0; q--)
            {
                InputPoints.Insert(InsertedPoints[q], i + 1);
            }

            i += InsertedPoints.Num(); // 跳过新插入的交点
            CountBefore += InsertedPoints.Num();
        }
    }

    //在二维坐标下分点 . 这里的输出是每个面上的二维坐标, FVector.z为0
    {
        for (FPointInfo Point : InputPoints)
        {
            for(int faceidx : Point.FaceIndex)
            {
                OutGroups[faceidx].Add(LocalSpace2Panel(faceidx, Point.WorldPos));
            }
        }

        //如何判断是否要加入三面顶点 ? 就看起点和终点所在边
        if(bAddNewPoint)
        {
            FVector LeftTop = FVector(0.0f, 0.0f, 0.0f);
            FVector RightTop = FVector(1.0f, 0.0f, 0.0f);
            FVector LeftBottom = FVector(0.0f, 1.0f, 0.0f);
            FVector RightBottom = FVector(1.0f, 1.0f, 0.0f);
            //先找到起点和终点 , 也就是入面点和出面点
            for (int i = 0; i < OutGroups.Num(); i++)
            {
                TArray<FVector>& Group = OutGroups[i];
                if(Group.Num() > 1)
                {
                    FVector InFace = FVector(-1.0f,-1.0f,-1.0f);
                    FVector OutFace = FVector(-1.0f, -1.0f, -1.0f);
                    for (int j = 0; j < Group.Num(); j++)
                    {
                        FVector Start = Group[j];
                        FVector End = Group[(j + 1) % Group.Num()];
                        bool IsStartOnEdge = IsPointOnEdge(Start);
                        bool IsEndOnEdge = IsPointOnEdge(End);
                        if (IsStartOnEdge && !IsEndOnEdge)
                        {
                            InFace = Start;
                        }else if(!IsStartOnEdge && IsEndOnEdge)
                        {
                            OutFace = End;
                        }
                        else if (IsStartOnEdge && IsEndOnEdge && !IsOnSameEdge(Start, End) && Group.Num() == 2)
                        {
                            InFace = Start;
                            OutFace = End;
                        }
                    }
                    check(!InFace.Equals(FVector(-1.0f, -1.0f, -1.0f)));
                    check(!OutFace.Equals(FVector(-1.0f, -1.0f, -1.0f)));
                    if((FMath::IsNearlyZero(InFace.X, Tolerance) && FMath::IsNearlyZero(OutFace.Y, Tolerance) && !InFace.Equals(LeftTop, Tolerance) && !OutFace.Equals(LeftTop, Tolerance))||
                        (FMath::IsNearlyZero(InFace.Y, Tolerance) && FMath::IsNearlyZero(OutFace.X, Tolerance) && !InFace.Equals(LeftTop, Tolerance) && !OutFace.Equals(LeftTop, Tolerance)))
                    {
                        AddIfCantFind(Group, LeftTop);
                    }else if((FMath::IsNearlyEqual(InFace.X, 1.0f, Tolerance) && FMath::IsNearlyZero(OutFace.Y, Tolerance) && !InFace.Equals(RightTop, Tolerance) && !OutFace.Equals(RightTop, Tolerance)) ||
                        (FMath::IsNearlyZero(InFace.Y, Tolerance) && FMath::IsNearlyEqual(OutFace.X, 1.0f, Tolerance) && !InFace.Equals(RightTop, Tolerance) && !OutFace.Equals(RightTop, Tolerance)))
                    {
                        AddIfCantFind(Group, RightTop);
                    }
                    else if((FMath::IsNearlyZero(InFace.X, Tolerance) && FMath::IsNearlyEqual(OutFace.Y, 1.0f, Tolerance) && !InFace.Equals(LeftBottom, Tolerance) && !OutFace.Equals(LeftBottom, Tolerance)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f, Tolerance) && FMath::IsNearlyZero(OutFace.X, Tolerance) && !InFace.Equals(LeftBottom, Tolerance) && !OutFace.Equals(LeftBottom, Tolerance)))
                    {
                        AddIfCantFind(Group, LeftBottom);
                    }
                    else if ((FMath::IsNearlyEqual(InFace.Y, 1.0f, Tolerance) && FMath::IsNearlyEqual(OutFace.X, 1.0f, Tolerance) && !InFace.Equals(RightBottom, Tolerance) && !OutFace.Equals(RightBottom, Tolerance)) ||
                        (FMath::IsNearlyEqual(InFace.X, 1.0f, Tolerance) && FMath::IsNearlyEqual(OutFace.Y, 1.0f, Tolerance) && !InFace.Equals(RightBottom, Tolerance) && !OutFace.Equals(RightBottom, Tolerance)))
                    {
                        AddIfCantFind(Group, RightBottom);
                    }
                    else if ((FMath::IsNearlyZero(InFace.Y, Tolerance) && FMath::IsNearlyEqual(OutFace.Y, 1.0f, Tolerance) && !InFace.Equals(RightTop, Tolerance) && !OutFace.Equals(RightBottom, Tolerance)) ||
                        (FMath::IsNearlyEqual(InFace.Y, 1.0f, Tolerance) && FMath::IsNearlyZero(OutFace.Y, Tolerance) && !InFace.Equals(RightBottom, Tolerance) && !OutFace.Equals(RightTop, Tolerance)))
                    {
                        if(i == 0)
                        {
                            AddIfCantFind(Group, RightTop);
                            AddIfCantFind(Group, RightBottom);
                        }
                        if(i == 1)
                        {
                            AddIfCantFind(Group, LeftTop);
                            AddIfCantFind(Group, LeftBottom);
                        }
                    }
                    //入点到出点从左到右横跨整幅图片的情况不可能出现, 而且要另外的信息确认是上部分被围还是下部分被围
                    //else if ((FMath::IsNearlyEqual(InFace.X, 0.0f) && FMath::IsNearlyEqual(OutFace.X, 1.0f) && !InFace.Equals(RightTop) && !OutFace.Equals(RightBottom)) ||
                    //    (FMath::IsNearlyEqual(InFace.X, 1.0f) && FMath::IsNearlyEqual(OutFace.X, 0.0f) && !InFace.Equals(RightTop) && !OutFace.Equals(RightBottom)))
                    //{
                    //    if (i == 0)
                    //    {
                    //        Group.Add(FVector(0.0f, 0.0f, 0.0f));
                    //        Group.Add(FVector(1.0f, 0.0f, 0.0f));
                    //    }
                    //    if (i == 1)
                    //    {
                    //        Group.Add(FVector(0.0f, 0.0f, 0.0f));
                    //        Group.Add(FVector(1.0f, 0.0f, 0.0f));
                    //    }
                    //}
                }else
                {
                    Group.Empty();
                }

            }
        }
        //int FaceCount = 0;
        //for (TArray<FVector> Group : OutGroups)
        //{
        //    //check(Group.Num() == 0 || Group.Num() >= 2);
        //    if (Group.Num() > 0)
        //        FaceCount++;
        //}
        //if(FaceCount >= 3)
        //{
        //    if(OutGroups[0].Num() > 0 && OutGroups[1].Num() > 0 && OutGroups[2].Num() > 0)
        //    {
        //        OutGroups[0].Add(LocalSpace2Panel(0, FVector(UE_SQRT_2, 0.0, 1.0)));
        //        OutGroups[1].Add(LocalSpace2Panel(1, FVector(UE_SQRT_2, 0.0, 1.0)));
        //        OutGroups[2].Add(LocalSpace2Panel(2, FVector(UE_SQRT_2, 0.0, 1.0)));
        //    }else if(OutGroups[0].Num() > 0 && OutGroups[1].Num() > 0 && OutGroups[3].Num() > 0)
        //    {
        //        OutGroups[0].Add(LocalSpace2Panel(0, FVector(UE_SQRT_2, 0.0, -1.0)));
        //        OutGroups[1].Add(LocalSpace2Panel(1, FVector(UE_SQRT_2, 0.0, -1.0)));
        //        OutGroups[3].Add(LocalSpace2Panel(3, FVector(UE_SQRT_2, 0.0, -1.0)));
        //    }
        //}

        for(TArray<FVector> Group : OutGroups)
        {
            float s = ComputePolygonArea2D(Group);
            if (s < Tolerance)
                Group.Empty();
        }
    }

    //在三维空间下根据平面分点
    {
        //只有生成了边界上的新点, 才需要划分点的区域
        //if (bAddNewPoint)
        //{
        //    // 根据分割出来的点, 生成归属于各个面的归属点
        //    TArray<int> IsSegGrouped;
        //    TArray<FSegment> SegStack;
        //    //下标i号元素为1表示InputPoints中的i为start,i+1为end的线段已经分好组
        //    IsSegGrouped.Init(0, InputPoints.Num());
        //    int CountGroupedSeg = 0;
        //    int idx = 0;
        //    bool FoundEntryPoint = false;
        //    for (int i = 0; i < InputPoints.Num() * 2; i++)
        //    {
        //        FSegment seg(InputPoints[i % InputPoints.Num()], InputPoints[(i + 1) % InputPoints.Num()]);
        //        TArray<int> SameFaces;
        //        check(IsSegHasSameFaces(seg, SameFaces));
        //        if (FoundEntryPoint)
        //        {
        //            //找到出线段, 出栈
        //            if (SameFaces.Num() == 1 && seg.PStart.FaceIndex.Num() == 1 && seg.PEnd.FaceIndex.Num() > 1)
        //            {
        //                TArray<FPointInfo> Group;
        //                FPointInfo EndPoint = seg.PEnd;
        //                while (SegStack.Num())
        //                {
        //                    Group.Add(SegStack.Last().PStart);
        //                    SegStack.Pop();
        //                }
        //                if ()
        //                    //找到了终点,就要重新找下一个起点
        //                    FoundEntryPoint = false;
        //            }
        //            else
        //            {
        //                SegStack.Push(seg);
        //            }
        //        }
        //        else
        //        {
        //            //如果没找到entry seg, 并且两个点的面一样(所在的所有面都一样) , 没找到entry seg , 就下一个
        //            if (IsSegSame(seg))
        //                continue;
        //            if (SameFaces.Num() == 1 && seg.PEnd.FaceIndex.Num() == 1 && seg.PStart.FaceIndex.Num() > 1)
        //            {   //找到入线段 , 入栈
        //                SegStack.Push(seg);
        //                //找到起点,就要找终点
        //                FoundEntryPoint = true;
        //            }
        //            else if (SameFaces.Num() == 1 && seg.PEnd.FaceIndex.Num() > 1 && seg.PStart.FaceIndex.Num() > 1)
        //            {//线段直接搭在两边或顶点和边上,需要额外加顶点
        //            }
        //        }
        //    }
        //}
    }
}

void CheckPointsFaces(TArray<FPointInfo>& InputPoints)
{
    TArray<FPlane> PlaneArray;
    PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    for(int i = 0; i < InputPoints.Num(); i++)
    {
        for(int j = 0; j < PlaneArray.Num(); j++)
        {
            if (IsPointOnPlane(InputPoints[i].WorldPos, PlaneArray[j]))
            {
                InputPoints[i].FaceIndex.Add(j);
            }
        }
    }
}


void AFisheyeCameraCS4::TestSplitPoints()
{
    TArray<FPointInfo> Input;

    //case0
    {
        //// 面 0: x - y = √2
        //Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 0), { 0 }));
        //Input.Add(FPointInfo(FVector(2, UE_SQRT_2, 0.5), { 0 }));
        //Input.Add(FPointInfo(FVector(1.5, UE_SQRT_2 - 0.5, -0.2), { 0 }));

        //// 面 1: x + y = √2
        //Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, -0.5), { 1 }));
        //Input.Add(FPointInfo(FVector(1, UE_SQRT_2 - 1, 0.2), { 1 }));
        //Input.Add(FPointInfo(FVector(UE_SQRT_2 - 0.5, 0.5, -0.3), { 1 }));

        //// 面 3: z = 1
        //Input.Add(FPointInfo(FVector(0, 0, 1), { 3 }));
        //Input.Add(FPointInfo(FVector(1, 0, 1), { 3 }));
        //Input.Add(FPointInfo(FVector(0, 1, 1), { 3 }));
    }

    //case1
    //{
    //    // 面 0: x - y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.5), { 0 }));

    //    // 面 1: x + y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.5), { 1 }));

    //    // 面 3: z = 1
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), { 3 }));
    //    
    //}

    //case2
    //{
    //    // 面 0: x - y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.5), { 0 }));

    //    //面 0 1 交线
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 0.5), { 0,1 }));

    //    // 面 1: x + y = √2
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.5), { 1 }));

    //    // 面 3: z = 1
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), { 3 }));

    //}

    //case3 : 最后两个点情况特殊 , 都在3面上 , 但是一个是13共线一个是03共线 , 这种情况下应该取中间的三面点作为分割点
    //bug, tbd
    //{
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, -1.0000), {}));
    //}

    //case4 : 最后两个点情况特殊 , 都在13共线面上 
    //{
    //    Input.Add(FPointInfo(FVector(0.7071, -0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, 0.5000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0.7071, 0.7071, -1.0000), {}));
    //}

    //case5 : 三个在交线上的点构成的三角形. 这里的问题和case3一样 , 问题在于这个时候已经不是生成新交点了 .
    //生成新交点代码能做的是不共面的点根据投影生成穿越多个面的连接线, 找到连接线和面之间的交线的交点 .
    //而这种情况只会发生在下是多个点位于不同的三个面 , 正好把顶点围起来了 . 这时候新增点的工作就结束了 ,
    //需要一个新的函数 , 也就是分割 . 简单的双面情况就是遍历点 , 找到正好处于边界的两个点 , 然后分割.
    //三面就是这种情况 , 需要先确定是哪个顶点 , 然后在遍历点. 找到入面点和出面点后 , 再加上顶点 , 就能分割成三个面
    //{
    //    Input.Add(FPointInfo(FVector(1.4142, 0.0000, 1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, -1.4142, -1.0000), {}));
    //}

    //case6: case5的极端情况
    //{
    //    Input.Add(FPointInfo(FVector(0.707, 0.0000, 1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, 1.4142, 0), {}));
    //    Input.Add(FPointInfo(FVector(0.707, 0.0000, -1.0000), {}));
    //    Input.Add(FPointInfo(FVector(0, -1.4142, 0), {}));
    //}

    //case7: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, -0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, -0.50000), {}));
    //}


    //case8: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, -UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, UE_SQRT_2 / 2, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 / 2, 0, -1), {}));
    //}

    //case9: 
    //{
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.5, -UE_SQRT_2 * 0.5, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.75, -UE_SQRT_2 * 0.25, 0.75000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.75, UE_SQRT_2 * 0.25, 0.750000), {}));
    //    Input.Add(FPointInfo(FVector(UE_SQRT_2 * 0.5, UE_SQRT_2 * 0.5, 0.50000), {}));
    //    Input.Add(FPointInfo(FVector(0.5, 0.5, -1), {}));
    //    Input.Add(FPointInfo(FVector(0.1, 0, -1), {}));
    //    Input.Add(FPointInfo(FVector(0.5, -0.5, -1), {}));
    //}

    //case10: 
    {
        Input.Add(FPointInfo(FVector(0, -UE_SQRT_2, 1.0000), {}));
        Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, 1.000), {}));
        Input.Add(FPointInfo(FVector(UE_SQRT_2, 0, -1.000), {}));
        Input.Add(FPointInfo(FVector(0, -UE_SQRT_2, -1.0000), {}));

    }

    CheckPointsFaces(Input);
    UE_LOG(LogTemp, Warning, TEXT("=== SplitPoints Before ==="));
    UE_LOG(LogTemp, Warning, TEXT("=== Input ==="));
    for (int i = 0; i < Input.Num(); i++)
    {
        const FVector& P = Input[i].WorldPos;
        UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), i, P.X, P.Y, P.Z);
        for (int j = 0; j < Input[i].FaceIndex.Num(); j++)
        {
            FVector local = LocalSpace2Panel(Input[i].FaceIndex[j], P);
            UE_LOG(LogTemp, Warning, TEXT("Local Planel[%d] coord (%.4f, %.4f, %.4f)"), Input[i].FaceIndex[j], local.X, local.Y, local.Z);
        }
    }
    TArray<TArray<FVector>> OutGroups;
    OutGroups.SetNum(4);
    SplitPoints(Input, OutGroups);

    // 打印输出
    if (OutGroups.Num() > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("=== SplitPoints Result ==="));
        UE_LOG(LogTemp, Warning, TEXT("=== Input ==="));
        for (int i = 0; i < Input.Num(); i++)
        {
            const FVector& P = Input[i].WorldPos;
            UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), i, P.X, P.Y, P.Z);
            for (int j = 0; j < Input[i].FaceIndex.Num(); j++)
            {
                FVector local = LocalSpace2Panel(Input[i].FaceIndex[j], P);
                UE_LOG(LogTemp, Warning, TEXT("Local Planel[%d] coord (%.4f, %.4f, %.4f)"), Input[i].FaceIndex[j], local.X, local.Y, local.Z);
            }
        }
        for(int i = 0; i < OutGroups.Num(); i++)
        {
            UE_LOG(LogTemp, Warning, TEXT("=== Plane[%d] ==="), i);
            for (int j = 0; j < OutGroups[i].Num(); j++)
            {
                const FVector& P = OutGroups[i][j];
                UE_LOG(LogTemp, Warning, TEXT("[%d] (%.4f, %.4f, %.4f)"), j, P.X, P.Y, P.Z);
            }
            UE_LOG(LogTemp, Warning, TEXT("Area is %.4f"), ComputePolygonArea2D(OutGroups[i]));
            
        }
        UE_LOG(LogTemp, Warning, TEXT("=========================="));
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("No output groups generated."));
    }
}











AFisheyeCameraCS4::AFisheyeCameraCS4(const FObjectInitializer &ObjectInitializer)
    : Super(ObjectInitializer)
{
    UE_LOG(LogTemp, Log, TEXT("in AFisheyeCameraCS4::AFisheyeCameraCS4"));
    // Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics; // After CameraManager's TG_PrePhysics.

        for (int i = 0; i < 4; ++i)
        {
            CaptureComponent2D.Add(CreateDefaultSubobject<USceneCaptureComponent2D>(
                FName(*FString::Printf(TEXT("AFisheyeCameraCS4SceneCaptureComponent2D_%d"), i))));
            CaptureComponent2D[i]->FOVAngle = 90;
            CaptureComponent2D[i]->SetupAttachment(RootComponent);
        }
        //Left
        CaptureComponent2D[0]->SetRelativeRotation(FRotator(0, -45, 0));
        //Right
        CaptureComponent2D[1]->SetRelativeRotation(FRotator(0, 45, 0));
        //Top
        CaptureComponent2D[2]->SetRelativeRotation(FRotator(90, 0, 45));
        //Bottom
        CaptureComponent2D[3]->SetRelativeRotation(FRotator(-90, 0, 45));

    ++FISHEYECS4_COUNTER;
    UE_LOG(LogTemp, Log, TEXT("ImageWidth %d, Radius %f, SampleDist %f,ProjectionModel %d, Layout %d"), 
        ImageWidth, Radius, SampleDist, ProjectionModel, Layout);

    UE_LOG(LogTemp, Log, TEXT("out AFisheyeCameraCS4::AFisheyeCameraCS4"));
}

void AFisheyeCameraCS4::BeginPlay()
{
    UE_LOG(LogTemp, Log, TEXT("in AFisheyeCameraCS4::BeginPlay()"));
    const bool bInForceLinearGamma = !bEnablePostProcessingEffects;

    for (int i = 0; i < 4; ++i)
    {
        CaptureRenderTarget.Add(NewObject<UTextureRenderTarget2D>(this));
        CaptureRenderTarget[i]->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
        CaptureRenderTarget[i]->SRGB = false;
        CaptureRenderTarget[i]->bAutoGenerateMips = true;
        CaptureRenderTarget[i]->AddressX = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->AddressY = TextureAddress::TA_Clamp;
        CaptureRenderTarget[i]->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        CaptureRenderTarget[i]->InitCustomFormat(ImageWidth, ImageWidth, PF_FloatRGBA, bInForceLinearGamma);
        if (bEnablePostProcessingEffects)
        {
            CaptureRenderTarget[i]->TargetGamma = TargetGamma;
        }
        

        //CaptureComponent2D.Add(NewObject<USceneCaptureComponent2D>(this));
        //CaptureComponent2D[i]->RegisterComponent();
        //CaptureComponent2D[i]->FOVAngle = 90;
        check(IsValid(CaptureComponent2D[i]) && !CaptureComponent2D[i]->IsPendingKill());
        CaptureComponent2D[i]->Deactivate();
        CaptureComponent2D[i]->TextureTarget = CaptureRenderTarget[i];
        CaptureComponent2D[i]->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
        CaptureComponent2D[i]->ShowFlags.Vignette = 0;
        CaptureComponent2D[i]->ShowFlags.Bloom = 0;
        //CaptureComponent2D[i]->ShowFlags.MotionBlur = 0;
        //CaptureComponent2D[i]->ShowFlags.Tonemapper = 0;
        CaptureComponent2D[i]->ShowFlags.EyeAdaptation = 0;
        //CaptureComponent2D[i]->ShowFlags.TemporalAA = 0;
        CaptureComponent2D[i]->ShowFlags.SkipTonemapper = 0;
        CaptureComponent2D[i]->UpdateContent();
        CaptureComponent2D[i]->Activate();
        //CaptureComponent2D[i]->SetupAttachment(RootComponent);

        FisheyeCameraCS4_local_ns::ConfigureShowFlags(CaptureComponent2D[i]->ShowFlags, bEnablePostProcessingEffects);
    }
    //Left
    CaptureComponent2D[0]->SetRelativeRotation(FRotator(0, -45, 0));
    //Right
    CaptureComponent2D[1]->SetRelativeRotation(FRotator(0, 45, 0));
    //Top
    CaptureComponent2D[2]->SetRelativeRotation(FRotator(90, 0, 45));
    //Bottom
    CaptureComponent2D[3]->SetRelativeRotation(FRotator(-90, 0, 45));

    FishEyeTexture = NewObject<UTextureRenderTarget2D>(this);
    FishEyeTexture->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    FishEyeTexture->bAutoGenerateMips = false;
    FishEyeTexture->InitCustomFormat(ImageWidth, ImageWidth, PF_FloatRGBA, !bEnablePostProcessingEffects);
    FishEyeTexture->UpdateResourceImmediate(true);

    FishEyeTextureLDR = NewObject<UTextureRenderTarget2D>(this);
    FishEyeTextureLDR->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    FishEyeTextureLDR->bAutoGenerateMips = false;
    FishEyeTextureLDR->InitCustomFormat(ImageWidth, ImageWidth, PF_B8G8R8A8, !bEnablePostProcessingEffects);
    FishEyeTextureLDR->UpdateResourceImmediate(true);

    int MipWidth = ImageWidth;
    for (int CurrentMipmapLevel = 0; CurrentMipmapLevel < MipLevel; ++CurrentMipmapLevel)
    {
        MipWidth =  FMath::DivideAndRoundUp(MipWidth, 2);
        //int oldWidth = ImageWidth >> CurrentMipmapLevel;
        //UE_LOG(LogTemp, Log, TEXT("oldWidth %d, MipWidth %d"), oldWidth, MipWidth);
        MipBloomRenderTarget.Add(NewObject<UTextureRenderTarget2D>(this));
        MipBloomRenderTarget[CurrentMipmapLevel]->ClearColor = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
        MipBloomRenderTarget[CurrentMipmapLevel]->bAutoGenerateMips = false;
        MipBloomRenderTarget[CurrentMipmapLevel]->InitCustomFormat(MipWidth, MipWidth, PF_FloatRGBA, !bEnablePostProcessingEffects);
        MipBloomRenderTarget[CurrentMipmapLevel]->UpdateResourceImmediate(true);
    }


    Radius = float(ImageWidth) / 2;
    SampleDist = 1.0 / (2.0 * float(SampleNum));

    FisheyeCS4CameraRenderingPtr = NewObject<UFisheyeCS4CameraRendering>(this);
    //FisheyeCS4CameraRenderingPtr->TestResourceArraySerialization();
    FisheyeCS4CameraRenderingPtr->CalPixelsRelationship(FIntPoint(ImageWidth, ImageWidth), 4, ProjectionModel, Layout);

    // Make sure that there is enough time in the render queue.
    UKismetSystemLibrary::ExecuteConsoleCommand(
        GetWorld(),
        FString("g.TimeoutForBlockOnRenderFence 300000"));

    // This ensures the camera is always spawning the rain drops in case the
    // weather was previously set to has rain
    GetEpisode().GetWeather()->NotifyWeather();
    Super::BeginPlay();
    UE_LOG(LogTemp, Log, TEXT("ImageWidth %d, Radius %f, SampleDist %f,ProjectionModel %d, Layout %d ,CaptureRenderTarget0 sizex %d"),
        ImageWidth, Radius, SampleDist, ProjectionModel, Layout, CaptureRenderTarget[0]->SizeX);
    UE_LOG(LogTemp, Log, TEXT("out AFisheyeCameraCS4::BeginPlay()"));

    //TestArea();
    TestSplitPoints();
}




void AFisheyeCameraCS4::TestArea()
{

    TArray<FPlane> PlaneArray;
    PlaneArray.Add(FPlane(1, -1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(1, 1, 0, UE_SQRT_2));
    PlaneArray.Add(FPlane(0, 0, 1, 1));
    PlaneArray.Add(FPlane(0, 0, 1, -1));

    //测试当全部点在同一面上,面积的计算,z轴不考虑
    {
    //    //case 0:所有点均在面上,没有在边上的三角形
    //    //TArray<FVector> P0;
    //    //P0.Add(FVector(0.2, 0.2, 0));
    //    //P0.Add(FVector(0.4, 0.2, 0));
    //    //P0.Add(FVector(0.2, 0.4, 0));
    //    //UE_LOG(LogTemp, Log, TEXT("P0 area: %f"), ComputePolygonArea2D(P0));

    //    //case 1:一个边在边缘上的四边形角形
    //    TArray<FVector> P1;
    //    P1.Add(FVector(0, 0, 0));
    //    P1.Add(FVector(0.5, 0, 0));
    //    P1.Add(FVector(0.5, 0.5, 0));
    //    P1.Add(FVector(0, 0.5, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P1 area: %f"), ComputePolygonArea2D(P1));

    //    //case 2:一个边在边缘上的四边形角形
    //    TArray<FVector> P2;
    //    P2.Add(FVector(0, 0, 0));
    //    P2.Add(FVector(0.5, 0, 0));
    //    P2.Add(FVector(1, 0.5, 0));
    //    P2.Add(FVector(0.5, 0.5, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P2 area: %f"), ComputePolygonArea2D(P2));

    //    //case 3:一个边在边缘上的正方形,另一个紧挨着的正方形
    //    TArray<FVector> P3;
    //    P3.Add(FVector(0, 0, 0));
    //    P3.Add(FVector(1, 0, 0));
    //    P3.Add(FVector(1, 0.5, 0));
    //    P3.Add(FVector(2, 0.5, 0));
    //    P3.Add(FVector(2, 1.5, 0));
    //    P3.Add(FVector(1, 1.5, 0));
    //    P3.Add(FVector(1, 1, 0));
    //    P3.Add(FVector(0, 1, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P3 area: %f"), ComputePolygonArea2D(P3));

    //    TArray<FVector> P4;
    //    P4.Add(FVector(0, 0, 0));
    //    P4.Add(FVector(1, 0, 0));
    //    P4.Add(FVector(1, 1, 0));
    //    P4.Add(FVector(0, 1, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P4 area (expected 1.0): %f"), ComputePolygonArea2D(P4));

    //    TArray<FVector> P5;
    //    P5.Add(FVector(0, 0, 0));
    //    P5.Add(FVector(1, 0, 0));
    //    P5.Add(FVector(0, 1, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P5 area (expected 0.5): %f"), ComputePolygonArea2D(P5));

    //    TArray<FVector> P6;
    //    P6.Add(FVector(0, 0, 0));
    //    P6.Add(FVector(2, 0, 0));
    //    P6.Add(FVector(2, 1, 0));
    //    P6.Add(FVector(1, 1, 0));
    //    P6.Add(FVector(1, 2, 0));
    //    P6.Add(FVector(0, 2, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P6 area (expected 3.0): %f"), ComputePolygonArea2D(P6));

    //    TArray<FVector> P7;
    //    P7.Add(FVector(0, 0, 0));
    //    P7.Add(FVector(1, -1, 0));
    //    P7.Add(FVector(2, 0, 0));
    //    P7.Add(FVector(1, 1, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P7 area (expected 2.0): %f"), ComputePolygonArea2D(P7));

    //    TArray<FVector> P8;
    //    P8.Add(FVector(0, 0, 0));
    //    P8.Add(FVector(2, 0, 0));
    //    P8.Add(FVector(1.5, 1, 0));
    //    P8.Add(FVector(0.5, 1, 0));
    //    UE_LOG(LogTemp, Log, TEXT("P8 area (expected 1.5): %f"), ComputePolygonArea2D(P8));

    //    TArray<FVector> P9;
    //    P9.Add(FVector(0, 0, 0));
    //    P9.Add(FVector(1, 1, 0));
    //    P9.Add(FVector(0, 2, 0));
    //    P9.Add(FVector(1, 0, 0));
    //    P9.Add(FVector(0, 1, 0));
    //    float Area = ComputePolygonArea2D(P9);
    //    if (Area < 0)
    //    {
    //        UE_LOG(LogTemp, Error, TEXT("P9 area is invalid (self-intersecting polygon)"));
    //    }
    //    else
    //    {
    //        UE_LOG(LogTemp, Warning, TEXT("P9 area: %f"), Area);
    //    }

    //    TArray<FVector> P10A;
    //    P10A.Add(FVector(0, 0, 0));
    //    P10A.Add(FVector(1, 0, 0));
    //    P10A.Add(FVector(1, 1, 0));
    //    P10A.Add(FVector(0, 1, 0));

    //    TArray<FVector> P10B;
    //    P10B.Add(FVector(1, 0, 0));
    //    P10B.Add(FVector(2, 0, 0));
    //    P10B.Add(FVector(2, 1, 0));
    //    P10B.Add(FVector(1, 1, 0));

    //    float Area10 = ComputePolygonArea2D(P10A) + ComputePolygonArea2D(P10B);
    //    UE_LOG(LogTemp, Log, TEXT("P10 area (expected 2.0): %f"), Area10);
    }

    //测试区域划分
    {
        // 共面测试数据
        TArray<FPointInfo> TestPoints_SingleFace;
        TestPoints_SingleFace.Add(FPointInfo(FVector(0, 0, 0), { 0 ,3}));
        TestPoints_SingleFace.Add(FPointInfo(FVector(100, 0, 0), { 0,1 }));
        TestPoints_SingleFace.Add(FPointInfo(FVector(100, 100, 0), { 0,1,2 }));
        TestPoints_SingleFace.Add(FPointInfo(FVector(0, 100, 0), { 0,2,3 }));

        TArray<TArray<FVector>> OutGroups_SingleFace;
        OutGroups_SingleFace.SetNum(4);
        SplitPointsByFace(TestPoints_SingleFace, OutGroups_SingleFace);

        // 打印
        for (int32 FaceIndex = 0; FaceIndex < OutGroups_SingleFace.Num(); ++FaceIndex)
        {
            const TArray<FVector>& Group = OutGroups_SingleFace[FaceIndex];
            if (Group.Num() > 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("FaceID = %d"), FaceIndex);
                for (const FVector& P : Group)
                {
                    UE_LOG(LogTemp, Warning, TEXT("   Point: %s"), *P.ToString());
                }
            }
        }

        //❌ 非共面测试数据（面0 和 面1 混合）
        TArray<FPointInfo> TestPoints_MultiFace;
        TestPoints_MultiFace.Add(FPointInfo(FVector(0, 0, 0), { 0 }));
        TestPoints_MultiFace.Add(FPointInfo(FVector(100, 0, 0), { 0, 1 }));
        TestPoints_MultiFace.Add(FPointInfo(FVector(100, 100, 100), { 1 }));
        TestPoints_MultiFace.Add(FPointInfo(FVector(0, 100, 100), { 1 }));

        TArray<TArray<FVector>> OutGroups_MultiFace;
        OutGroups_MultiFace.SetNum(4);
        SplitPointsByFace(TestPoints_MultiFace, OutGroups_MultiFace);

        // 打印
        if (OutGroups_MultiFace.Num() == 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("MultiFace: 分裂逻辑未实现，未生成任何面"));
        }
        else
        {
            for (int32 FaceIndex = 0; FaceIndex < OutGroups_MultiFace.Num(); ++FaceIndex)
            {
                const TArray<FVector>& Group = OutGroups_MultiFace[FaceIndex];
                if (Group.Num() > 0)
                {
                    UE_LOG(LogTemp, Warning, TEXT("MultiFace: FaceID = %d"), FaceIndex);
                    for (const FVector& P : Group)
                    {
                        UE_LOG(LogTemp, Warning, TEXT("   Point: %s"), *P.ToString());
                    }
                }
            }
        }


    }
}

void AFisheyeCameraCS4::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    // Add the view information every tick. Its only used for one tick and then
    // removed by the streamer.
    // IStreamingManager::Get().AddViewInformation(
    //     CaptureComponent2D->GetComponentLocation(),
    //     ImageWidth,
    //     ImageWidth / FMath::Tan(CaptureComponent2D->FOVAngle));

    FDateTime Time = FDateTime::Now();
    int64 Timestamp = Time.ToUnixTimestamp();
    FString TimestampStr = FString::FromInt(Timestamp);

    //保存4个2D图片
    //for (int i = 0; i < 4; i++)
    //{
    //    FString SaveFileName = FPaths::ProjectSavedDir();

    //    SaveFileName.Append(FString("FishEyeCS4SplitNO"));
    //    SaveFileName.Append(FString::FromInt(i));
    //    SaveFileName.Append(TimestampStr);
    //    SaveFileName.Append(".jpg");
    //    ScreenshotToImage2D(SaveFileName, CaptureRenderTarget[i]);
    //}
    //FString SaveFileName = FPaths::ProjectSavedDir();
    //SaveFileName.Append(FString("FishEyeCS4"));
    //SaveFileName.Append(TimestampStr);
    //SaveFileName.Append(".jpg");
    auto &Setting = CaptureComponent2D[0]->PostProcessSettings;
    TArray<UFisheyeCS4CameraRendering::FBloomStage> BloomStages =
    {
        { Setting.Bloom6Size, Setting.Bloom6Tint },
        { Setting.Bloom5Size, Setting.Bloom5Tint },
        { Setting.Bloom4Size, Setting.Bloom4Tint },
        { Setting.Bloom3Size, Setting.Bloom3Tint },
        { Setting.Bloom2Size, Setting.Bloom2Tint },
        { Setting.Bloom1Size, Setting.Bloom1Tint }
    };
    FisheyeCS4CameraRenderingPtr->UseComputeShaderArray(CaptureRenderTarget, FishEyeTexture, FishEyeTextureLDR, MipBloomRenderTarget, BloomStages, 4, ProjectionModel, Layout);

    //ScreenshotToImage2D(SaveFileName, FishEyeTexture);
    SendFisheyeCameraCSPixelsInRenderThread(*this);
}

void AFisheyeCameraCS4::ScreenshotToImage2D(const FString& InImagePath, UTextureRenderTarget2D* TextureTarget)
{

    if (TextureTarget)
    {
        //auto start = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
        FTextureRenderTargetResource* TextureRenderTargetResource = TextureTarget->GameThread_GetRenderTargetResource();
        //auto mid = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
        int32 Width = TextureTarget->SizeX;
        int32 Height = TextureTarget->SizeY;
        if (Width > 0 && Height > 0) {
            TArray<FColor> OutData;
            TextureRenderTargetResource->ReadPixels(OutData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), FIntRect(0, 0, Width, Height));
            //auto mid2 = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
            ColorToImage(InImagePath, OutData, Width, Height);
            //auto mid3 = FDateTime::Now().GetTimeOfDay().GetTotalMilliseconds();
            //UE_LOG(LogTemp, Warning, TEXT("mid - start : %f, mid2-mid %f, mid3-mid2 %f"), mid - start, mid2 - mid,mid3-mid2);
        }
    }
    else {
        UE_LOG(LogTemp, Warning, TEXT("NO CaptureComponent2D->TextureTarget"));
    }
}

void AFisheyeCameraCS4::ColorToImage(const FString& InImagePath, TArray<FColor> InColor, int32 InWidth, int32 InHeight)
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

void AFisheyeCameraCS4::SendFisheyeCameraCSPixelsInRenderThread(AFisheyeCameraCS4 &Sensor)
{
    check(Sensor.FishEyeTexture != nullptr);
    // Enqueue a command in the render-thread that will write the image buffer to
    // the data stream. The stream is created in the capture thus executed in the
    // game-thread.

    ENQUEUE_RENDER_COMMAND(FWriteFisheyeCameraCSPixelsToBuffer_SendPixelsInRenderThread)
        (
            [&Sensor, Stream = this->GetDataStream(Sensor)](auto &InRHICmdList) mutable
    {
        if (!Sensor.IsPendingKill())
        {

            auto t1 = std::chrono::system_clock::now();

            auto Buffer = Stream.PopBufferFromPool();

            auto t2 = std::chrono::system_clock::now();

            Sensor.WriteFisheyeCameraCSPixelsToBuffer(
                Buffer,
                carla::sensor::SensorRegistry::get<AFisheyeCameraCS4 *>::type::header_offset,
                Sensor,
                InRHICmdList);

            auto t3 = std::chrono::system_clock::now();

            Stream.Send(Sensor, std::move(Buffer));

            auto t4 = std::chrono::system_clock::now();
            std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
            std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
            std::chrono::duration<double> elapsed_seconds_3 = t4 - t3;
            //WritePixelsToBuffer��ʱ5ms
            //UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time SendFisheyeCameraCSPixelsInRenderThread  , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count(), elapsed_seconds_3.count());
        }
    }
    );
}

void AFisheyeCameraCS4::WriteFisheyeCameraCSPixelsToBuffer(
    carla::Buffer &Buffer,
    uint32 Offset,
    AFisheyeCameraCS4 &Sensor,
    FRHICommandListImmediate &InRHICmdList)
{

    check(IsInRenderingThread());

    auto t1 = std::chrono::system_clock::now();

    //#if CARLA_WITH_VULKAN_SUPPORT == 1
    //    if (IsVulkanPlatform(GMaxRHIShaderPlatform))
    //    {
    //        UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsVulkanPlatform"));
    //        WritePixelsToBuffer_Vulkan(RenderTarget, Buffer, Offset, InRHICmdList);
    //        return;
    //    }
    //#endif // CARLA_WITH_VULKAN_SUPPORT

    FRHITexture2D *Texture = Sensor.FishEyeTextureLDR->GetRenderTargetResource()->GetRenderTargetTexture();
    checkf(Texture != nullptr, TEXT("AFisheyeCameraCS4::WriteFisheyeCameraCSPixelsToBuffer: UTextureRenderTarget2D missing render target texture"));

    const uint32 BytesPerPixel = 4u; // PF_R8G8B8A8
    const uint32 Width = Texture->GetSizeX();
    const uint32 Height = Texture->GetSizeY();
    const uint32 ExpectedStride = Width * BytesPerPixel;

    uint32 SrcStride;
    // 这个函数结束后Lock自动析构 , 就释放了RHILockTexture2D锁
    uint8 *Source = (reinterpret_cast<uint8*>(RHILockTexture2D(Texture, 0, RLM_ReadOnly, SrcStride, false)));


    auto t2 = std::chrono::system_clock::now();

#ifdef PLATFORM_WINDOWS
    // JB: Direct 3D uses additional rows in the buffer, so we need check the
    // result stride from the lock:
    //d3d平台单独处理是因为每一行末尾会有额外的数据 , 而其他api没有 , 所以这里可以整块直接复制
    if (IsD3DPlatform(GMaxRHIShaderPlatform, false) && (ExpectedStride != SrcStride))
    {
        //UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( IsD3DPlatform"));
        Buffer.reset(Offset + ExpectedStride * Height);
        auto DstRow = Buffer.begin() + Offset;
        const uint8 *SrcRow = Source;
        for (uint32 Row = 0u; Row < Height; ++Row)
        {
            //每一次只从SrcRow拷贝ExpectedStride长度到DstRow , 说明ExpectedStride < SrcStride ,
            //而且说明SrcStride比ExpectedStride多出来的那一截是在尾部 , 可以抛弃
            FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
            DstRow += ExpectedStride;
            SrcRow += SrcStride;
        }
    }
    else
#endif // PLATFORM_WINDOWS
    {
        //UE_LOG(LogTemp, Warning, TEXT("Jarvan FPixelReader::WritePixelsToBuffer( Buffer.copy_from"));
        check(ExpectedStride == SrcStride);
        //const uint8 *Source = Lock.Source;
        //这里会创建boost::asio::buffer里的const_buffer , 不可修改buffer里的数据
        Buffer.copy_from(Offset, Source, ExpectedStride * Height);
    }
    //test 1
    //Buffer.reset(Offset + ExpectedStride * Height);
    //auto DstRow = Buffer.begin() + Offset;
    //const uint8 *SrcRow = Source;
    //for (uint32 Row = 0u; Row < Height; ++Row)
    //{
    //    //每一次只从SrcRow拷贝ExpectedStride长度到DstRow , 说明ExpectedStride < SrcStride ,
    //    //而且说明SrcStride比ExpectedStride多出来的那一截是在尾部 , 可以抛弃
    //    FMemory::Memcpy(DstRow, SrcRow, ExpectedStride);
    //    DstRow += ExpectedStride;
    //    SrcRow += SrcStride;
    //}

    //test 2
    //Buffer.copy_from(Offset, Source, ExpectedStride * Height);

    auto t3 = std::chrono::system_clock::now();
    std::chrono::duration<double> elapsed_seconds_1 = t2 - t1;
    std::chrono::duration<double> elapsed_seconds_2 = t3 - t2;
    //  LockTexture Lock(Texture, SrcStride);耗时3ms
    RHIUnlockTexture2D(Texture, 0, false);
    //UE_LOG(LogTemp, Warning, TEXT("Jarvan cost time WritePixelsToBuffer , %.8lf, %.8lf, %.8lf"), elapsed_seconds_1.count(), elapsed_seconds_2.count());
}


FActorDefinition AFisheyeCameraCS4::GetSensorDefinition()
{
    constexpr bool bEnableModifyingPostProcessEffects = true;
    return UActorBlueprintFunctionLibrary::MakeCameraDefinition(
        TEXT("fisheyecs4"),
        bEnableModifyingPostProcessEffects);
}

void AFisheyeCameraCS4::Set(const FActorDescription &Description)
{
    UE_LOG(LogTemp, Log, TEXT("in AFisheyeCameraCS4::Set"));
    Super::Set(Description);
    //djw tbd
    UActorBlueprintFunctionLibrary::SetCamera(Description, this);
    UE_LOG(LogTemp, Log, TEXT("out AFisheyeCameraCS4::Set"));
}

void AFisheyeCameraCS4::SetImageSize(int Width)
{
    ImageWidth = Width;
    Radius = float(ImageWidth) / 2;
}

void AFisheyeCameraCS4::SetSSAA(int Num)
{
    SampleNum = Num;
    SampleDist = 1.0 / (2.0 * float(SampleNum));
}

void AFisheyeCameraCS4::SetProjectionModel(int Model)
{
    ProjectionModel = Model;
}

void AFisheyeCameraCS4::SetLayout(int layout)
{
    Layout = layout;
}

void AFisheyeCameraCS4::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Super::EndPlay(EndPlayReason);
    FISHEYECS4_COUNTER = 0u;
}

// =============================================================================
// -- Local static functions implementations -----------------------------------
// =============================================================================

namespace FisheyeCameraCS4_local_ns {

    static void SetCameraDefaultOverrides(USceneCaptureComponent2D &CaptureComponent2D)
    {
        auto &PostProcessSettings = CaptureComponent2D.PostProcessSettings;

        // Exposure
        PostProcessSettings.bOverride_AutoExposureMethod = true;
        PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PostProcessSettings.bOverride_AutoExposureBias = true;
        PostProcessSettings.bOverride_AutoExposureMinBrightness = true;
        PostProcessSettings.bOverride_AutoExposureMaxBrightness = true;
        PostProcessSettings.bOverride_AutoExposureSpeedUp = true;
        PostProcessSettings.bOverride_AutoExposureSpeedDown = true;
        PostProcessSettings.bOverride_AutoExposureCalibrationConstant = true;

        // Camera
        PostProcessSettings.bOverride_CameraShutterSpeed = true;
        PostProcessSettings.bOverride_CameraISO = true;
        PostProcessSettings.bOverride_DepthOfFieldFstop = true;
        PostProcessSettings.bOverride_DepthOfFieldMinFstop = true;
        PostProcessSettings.bOverride_DepthOfFieldBladeCount = true;

        // Film (Tonemapper)
        PostProcessSettings.bOverride_FilmSlope = true;
        PostProcessSettings.bOverride_FilmToe = true;
        PostProcessSettings.bOverride_FilmShoulder = true;
        PostProcessSettings.bOverride_FilmWhiteClip = true;
        PostProcessSettings.bOverride_FilmBlackClip = true;

        // Motion blur
        PostProcessSettings.bOverride_MotionBlurAmount = true;
        PostProcessSettings.MotionBlurAmount = 0.45f;
        PostProcessSettings.bOverride_MotionBlurMax = true;
        PostProcessSettings.MotionBlurMax = 0.35f;
        PostProcessSettings.bOverride_MotionBlurPerObjectSize = true;
        PostProcessSettings.MotionBlurPerObjectSize = 0.1f;

        // Color Grading
        PostProcessSettings.bOverride_WhiteTemp = true;
        PostProcessSettings.bOverride_WhiteTint = true;

        // Chromatic Aberration
        PostProcessSettings.bOverride_SceneFringeIntensity = true;
        PostProcessSettings.bOverride_ChromaticAberrationStartOffset = true;

        // Ambient Occlusion
        PostProcessSettings.bOverride_AmbientOcclusionIntensity = true;
        PostProcessSettings.AmbientOcclusionIntensity = 0.5f;
        PostProcessSettings.bOverride_AmbientOcclusionRadius = true;
        PostProcessSettings.AmbientOcclusionRadius = 100.0f;
        PostProcessSettings.bOverride_AmbientOcclusionStaticFraction = true;
        PostProcessSettings.AmbientOcclusionStaticFraction = 1.0f;
        PostProcessSettings.bOverride_AmbientOcclusionFadeDistance = true;
        PostProcessSettings.AmbientOcclusionFadeDistance = 50000.0f;
        PostProcessSettings.bOverride_AmbientOcclusionPower = true;
        PostProcessSettings.AmbientOcclusionPower = 2.0f;
        PostProcessSettings.bOverride_AmbientOcclusionBias = true;
        PostProcessSettings.AmbientOcclusionBias = 3.0f;
        PostProcessSettings.bOverride_AmbientOcclusionQuality = true;
        PostProcessSettings.AmbientOcclusionQuality = 100.0f;

        // Bloom
        PostProcessSettings.bOverride_BloomMethod = true;
        PostProcessSettings.BloomMethod = EBloomMethod::BM_SOG;
        PostProcessSettings.bOverride_BloomIntensity = true;
        PostProcessSettings.BloomIntensity = 0.3f;
        PostProcessSettings.bOverride_BloomThreshold = true;
        PostProcessSettings.BloomThreshold = -1.0f;
    }

    // Remove the show flags that might interfere with post-processing effects
    // like depth and semantic segmentation.
    static void ConfigureShowFlags(FEngineShowFlags &ShowFlags, bool bPostProcessing)
    {
        if (bPostProcessing)
        {
            ShowFlags.EnableAdvancedFeatures();
            ShowFlags.SetMotionBlur(true);
            return;
        }

        ShowFlags.SetAmbientOcclusion(false);
        ShowFlags.SetAntiAliasing(false);
        ShowFlags.SetVolumetricFog(false); // ShowFlags.SetAtmosphericFog(false);
        // ShowFlags.SetAudioRadius(false);
        // ShowFlags.SetBillboardSprites(false);
        ShowFlags.SetBloom(false);
        // ShowFlags.SetBounds(false);
        // ShowFlags.SetBrushes(false);
        // ShowFlags.SetBSP(false);
        // ShowFlags.SetBSPSplit(false);
        // ShowFlags.SetBSPTriangles(false);
        // ShowFlags.SetBuilderBrush(false);
        // ShowFlags.SetCameraAspectRatioBars(false);
        // ShowFlags.SetCameraFrustums(false);
        ShowFlags.SetCameraImperfections(false);
        ShowFlags.SetCameraInterpolation(false);
        // ShowFlags.SetCameraSafeFrames(false);
        // ShowFlags.SetCollision(false);
        // ShowFlags.SetCollisionPawn(false);
        // ShowFlags.SetCollisionVisibility(false);
        ShowFlags.SetColorGrading(false);
        // ShowFlags.SetCompositeEditorPrimitives(false);
        // ShowFlags.SetConstraints(false);
        // ShowFlags.SetCover(false);
        // ShowFlags.SetDebugAI(false);
        // ShowFlags.SetDecals(false);
        // ShowFlags.SetDeferredLighting(false);
        ShowFlags.SetDepthOfField(false);
        ShowFlags.SetDiffuse(false);
        ShowFlags.SetDirectionalLights(false);
        ShowFlags.SetDirectLighting(false);
        // ShowFlags.SetDistanceCulledPrimitives(false);
        // ShowFlags.SetDistanceFieldAO(false);
        // ShowFlags.SetDistanceFieldGI(false);
        ShowFlags.SetDynamicShadows(false);
        // ShowFlags.SetEditor(false);
        ShowFlags.SetEyeAdaptation(false);
        ShowFlags.SetFog(false);
        // ShowFlags.SetGame(false);
        // ShowFlags.SetGameplayDebug(false);
        // ShowFlags.SetGBufferHints(false);
        ShowFlags.SetGlobalIllumination(false);
        ShowFlags.SetGrain(false);
        // ShowFlags.SetGrid(false);
        // ShowFlags.SetHighResScreenshotMask(false);
        // ShowFlags.SetHitProxies(false);
        ShowFlags.SetHLODColoration(false);
        ShowFlags.SetHMDDistortion(false);
        // ShowFlags.SetIndirectLightingCache(false);
        // ShowFlags.SetInstancedFoliage(false);
        // ShowFlags.SetInstancedGrass(false);
        // ShowFlags.SetInstancedStaticMeshes(false);
        // ShowFlags.SetLandscape(false);
        // ShowFlags.SetLargeVertices(false);
        ShowFlags.SetLensFlares(false);
        ShowFlags.SetLevelColoration(false);
        ShowFlags.SetLightComplexity(false);
        ShowFlags.SetLightFunctions(false);
        ShowFlags.SetLightInfluences(false);
        ShowFlags.SetLighting(false);
        ShowFlags.SetLightMapDensity(false);
        ShowFlags.SetLightRadius(false);
        ShowFlags.SetLightShafts(false);
        // ShowFlags.SetLOD(false);
        ShowFlags.SetLODColoration(false);
        // ShowFlags.SetMaterials(false);
        // ShowFlags.SetMaterialTextureScaleAccuracy(false);
        // ShowFlags.SetMeshEdges(false);
        // ShowFlags.SetMeshUVDensityAccuracy(false);
        // ShowFlags.SetModeWidgets(false);
        ShowFlags.SetMotionBlur(false);
        // ShowFlags.SetNavigation(false);
        ShowFlags.SetOnScreenDebug(false);
        // ShowFlags.SetOutputMaterialTextureScales(false);
        // ShowFlags.SetOverrideDiffuseAndSpecular(false);
        // ShowFlags.SetPaper2DSprites(false);
        ShowFlags.SetParticles(false);
        // ShowFlags.SetPivot(false);
        ShowFlags.SetPointLights(false);
        // ShowFlags.SetPostProcessing(false);
        // ShowFlags.SetPostProcessMaterial(false);
        // ShowFlags.SetPrecomputedVisibility(false);
        // ShowFlags.SetPrecomputedVisibilityCells(false);
        // ShowFlags.SetPreviewShadowsIndicator(false);
        // ShowFlags.SetPrimitiveDistanceAccuracy(false);
        ShowFlags.SetPropertyColoration(false);
        // ShowFlags.SetQuadOverdraw(false);
        // ShowFlags.SetReflectionEnvironment(false);
        // ShowFlags.SetReflectionOverride(false);
        ShowFlags.SetRefraction(false);
        // ShowFlags.SetRendering(false);
        ShowFlags.SetSceneColorFringe(false);
        // ShowFlags.SetScreenPercentage(false);
        ShowFlags.SetScreenSpaceAO(false);
        ShowFlags.SetScreenSpaceReflections(false);
        // ShowFlags.SetSelection(false);
        // ShowFlags.SetSelectionOutline(false);
        // ShowFlags.SetSeparateTranslucency(false);
        // ShowFlags.SetShaderComplexity(false);
        // ShowFlags.SetShaderComplexityWithQuadOverdraw(false);
        // ShowFlags.SetShadowFrustums(false);
        // ShowFlags.SetSkeletalMeshes(false);
        // ShowFlags.SetSkinCache(false);
        ShowFlags.SetSkyLighting(false);
        // ShowFlags.SetSnap(false);
        // ShowFlags.SetSpecular(false);
        // ShowFlags.SetSplines(false);
        ShowFlags.SetSpotLights(false);
        // ShowFlags.SetStaticMeshes(false);
        ShowFlags.SetStationaryLightOverlap(false);
        // ShowFlags.SetStereoRendering(false);
        // ShowFlags.SetStreamingBounds(false);
        ShowFlags.SetSubsurfaceScattering(false);
        // ShowFlags.SetTemporalAA(false);
        // ShowFlags.SetTessellation(false);
        // ShowFlags.SetTestImage(false);
        // ShowFlags.SetTextRender(false);
        // ShowFlags.SetTexturedLightProfiles(false);
        ShowFlags.SetTonemapper(false);
        // ShowFlags.SetTranslucency(false);
        // ShowFlags.SetVectorFields(false);
        // ShowFlags.SetVertexColors(false);
        // ShowFlags.SetVignette(false);
        // ShowFlags.SetVisLog(false);
        // ShowFlags.SetVisualizeAdaptiveDOF(false);
        // ShowFlags.SetVisualizeBloom(false);
        ShowFlags.SetVisualizeBuffer(false);
        ShowFlags.SetVisualizeDistanceFieldAO(false);
        ShowFlags.SetVisualizeDistanceFieldGI(false);
        ShowFlags.SetVisualizeDOF(false);
        ShowFlags.SetVisualizeHDR(false);
        ShowFlags.SetVisualizeLightCulling(false);
        ShowFlags.SetVisualizeLPV(false);
        ShowFlags.SetVisualizeMeshDistanceFields(false);
        ShowFlags.SetVisualizeMotionBlur(false);
        ShowFlags.SetVisualizeOutOfBoundsPixels(false);
        ShowFlags.SetVisualizeSenses(false);
        ShowFlags.SetVisualizeShadingModels(false);
        ShowFlags.SetVisualizeSSR(false);
        ShowFlags.SetVisualizeSSS(false);
        // ShowFlags.SetVolumeLightingSamples(false);
        // ShowFlags.SetVolumes(false);
        // ShowFlags.SetWidgetComponents(false);
        // ShowFlags.SetWireframe(false);
    }

} // namespace SceneCaptureSensor_local_ns



void AFisheyeCameraCS4::SetExposureMethod(EAutoExposureMethod Method)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMethod = Method;
    }
}

EAutoExposureMethod AFisheyeCameraCS4::GetExposureMethod() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMethod;
}

void AFisheyeCameraCS4::SetExposureCompensation(float Compensation)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureBias = Compensation;
    }
}

float AFisheyeCameraCS4::GetExposureCompensation() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureBias;
}

void AFisheyeCameraCS4::SetShutterSpeed(float Speed)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraShutterSpeed = Speed;
    }
}

float AFisheyeCameraCS4::GetShutterSpeed() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraShutterSpeed;
}

void AFisheyeCameraCS4::SetISO(float ISO)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.CameraISO = ISO;
    }
}

float AFisheyeCameraCS4::GetISO() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.CameraISO;
}

void AFisheyeCameraCS4::SetAperture(float Aperture)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFstop = Aperture;
    }
}

float AFisheyeCameraCS4::GetAperture() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFstop;
}

void AFisheyeCameraCS4::SetFocalDistance(float Distance)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldFocalDistance = Distance;
    }
}

float AFisheyeCameraCS4::GetFocalDistance() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldFocalDistance;
}

void AFisheyeCameraCS4::SetDepthBlurAmount(float Amount)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurAmount = Amount;
    }
}

float AFisheyeCameraCS4::GetDepthBlurAmount() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurAmount;
}

void AFisheyeCameraCS4::SetDepthBlurRadius(float Radius)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldDepthBlurRadius = Radius;
    }
}

float AFisheyeCameraCS4::GetDepthBlurRadius() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldDepthBlurRadius;
}

void AFisheyeCameraCS4::SetDepthOfFieldMinFstop(float MinFstop)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldMinFstop = MinFstop;
    }
}

float AFisheyeCameraCS4::GetDepthOfFieldMinFstop() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldMinFstop;
}

void AFisheyeCameraCS4::SetBladeCount(int Count)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.DepthOfFieldBladeCount = Count;
    }
}

int AFisheyeCameraCS4::GetBladeCount() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.DepthOfFieldBladeCount;
}

void AFisheyeCameraCS4::SetFilmSlope(float Slope)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmSlope = Slope;
    }
}

float AFisheyeCameraCS4::GetFilmSlope() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmSlope;
}

void AFisheyeCameraCS4::SetFilmToe(float Toe)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmToe = Toe; // FilmToeAmount?
    }
}

float AFisheyeCameraCS4::GetFilmToe() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmToe;
}

void AFisheyeCameraCS4::SetFilmShoulder(float Shoulder)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmShoulder = Shoulder;
    }
}

float AFisheyeCameraCS4::GetFilmShoulder() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmShoulder;
}

void AFisheyeCameraCS4::SetFilmBlackClip(float BlackClip)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmBlackClip = BlackClip;
    }
}

float AFisheyeCameraCS4::GetFilmBlackClip() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmBlackClip;
}

void AFisheyeCameraCS4::SetFilmWhiteClip(float WhiteClip)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.FilmWhiteClip = WhiteClip;
    }
}

float AFisheyeCameraCS4::GetFilmWhiteClip() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.FilmWhiteClip;
}

void AFisheyeCameraCS4::SetExposureMinBrightness(float Brightness)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMinBrightness = Brightness;
    }
}

float AFisheyeCameraCS4::GetExposureMinBrightness() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMinBrightness;
}

void AFisheyeCameraCS4::SetExposureMaxBrightness(float Brightness)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureMaxBrightness = Brightness;
    }
}

float AFisheyeCameraCS4::GetExposureMaxBrightness() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureMaxBrightness;
}

void AFisheyeCameraCS4::SetExposureSpeedDown(float Speed)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedDown = Speed;
    }
}

float AFisheyeCameraCS4::GetExposureSpeedDown() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedDown;
}

void AFisheyeCameraCS4::SetExposureSpeedUp(float Speed)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureSpeedUp = Speed;
    }
}

float AFisheyeCameraCS4::GetExposureSpeedUp() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureSpeedUp;
}

void AFisheyeCameraCS4::SetExposureCalibrationConstant(float Constant)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.AutoExposureCalibrationConstant = Constant;
    }
}

float AFisheyeCameraCS4::GetExposureCalibrationConstant() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.AutoExposureCalibrationConstant;
}

void AFisheyeCameraCS4::SetMotionBlurIntensity(float Intensity)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurAmount = Intensity;
    }
}

float AFisheyeCameraCS4::GetMotionBlurIntensity() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurAmount;
}

void AFisheyeCameraCS4::SetMotionBlurMaxDistortion(float MaxDistortion)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurMax = MaxDistortion;
    }
}

float AFisheyeCameraCS4::GetMotionBlurMaxDistortion() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurMax;
}

void AFisheyeCameraCS4::SetMotionBlurMinObjectScreenSize(float ScreenSize)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.MotionBlurPerObjectSize = ScreenSize;
    }
}

float AFisheyeCameraCS4::GetMotionBlurMinObjectScreenSize() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.MotionBlurPerObjectSize;
}

void AFisheyeCameraCS4::SetWhiteTemp(float Temp)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTemp = Temp;
    }
}

float AFisheyeCameraCS4::GetWhiteTemp() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTemp;
}

void AFisheyeCameraCS4::SetWhiteTint(float Tint)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.WhiteTint = Tint;
    }
}

float AFisheyeCameraCS4::GetWhiteTint() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.WhiteTint;
}

void AFisheyeCameraCS4::SetChromAberrIntensity(float Intensity)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.SceneFringeIntensity = Intensity;
    }
}

float AFisheyeCameraCS4::GetChromAberrIntensity() const
{
    check(CaptureComponent2D.Num() != 0);
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.SceneFringeIntensity;
}

void AFisheyeCameraCS4::SetChromAberrOffset(float Offset)
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
        CaptureComponent2D[i]->PostProcessSettings.ChromaticAberrationStartOffset = Offset;
    }
}

float AFisheyeCameraCS4::GetChromAberrOffset() const
{
    for (int i = 0; i < 4; i++)
    {
        check(CaptureComponent2D[i] != nullptr);
    }
    return CaptureComponent2D[0]->PostProcessSettings.ChromaticAberrationStartOffset;
}