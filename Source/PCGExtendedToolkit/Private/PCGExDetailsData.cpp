﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "PCGExDetailsData.h"

namespace PCGExDetails
{
	TSharedPtr<FDistances> MakeDistances(const EPCGExDistance Source, const EPCGExDistance Target, const bool bOverlapIsZero)
	{
		if (Source == EPCGExDistance::None || Target == EPCGExDistance::None)
		{
			return MakeShared<TDistances<EPCGExDistance::None, EPCGExDistance::None>>();
		}
		if (Source == EPCGExDistance::Center)
		{
			if (Target == EPCGExDistance::Center) { return MakeShared<TDistances<EPCGExDistance::Center, EPCGExDistance::Center>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::SphereBounds) { return MakeShared<TDistances<EPCGExDistance::Center, EPCGExDistance::SphereBounds>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::BoxBounds) { return MakeShared<TDistances<EPCGExDistance::Center, EPCGExDistance::BoxBounds>>(bOverlapIsZero); }
		}
		else if (Source == EPCGExDistance::SphereBounds)
		{
			if (Target == EPCGExDistance::Center) { return MakeShared<TDistances<EPCGExDistance::SphereBounds, EPCGExDistance::Center>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::SphereBounds) { return MakeShared<TDistances<EPCGExDistance::SphereBounds, EPCGExDistance::SphereBounds>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::BoxBounds) { return MakeShared<TDistances<EPCGExDistance::SphereBounds, EPCGExDistance::BoxBounds>>(bOverlapIsZero); }
		}
		else if (Source == EPCGExDistance::BoxBounds)
		{
			if (Target == EPCGExDistance::Center) { return MakeShared<TDistances<EPCGExDistance::BoxBounds, EPCGExDistance::Center>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::SphereBounds) { return MakeShared<TDistances<EPCGExDistance::BoxBounds, EPCGExDistance::SphereBounds>>(bOverlapIsZero); }
			if (Target == EPCGExDistance::BoxBounds) { return MakeShared<TDistances<EPCGExDistance::BoxBounds, EPCGExDistance::BoxBounds>>(bOverlapIsZero); }
		}

		return nullptr;
	}

	TSharedPtr<FDistances> MakeNoneDistances()
	{
		return MakeShared<TDistances<EPCGExDistance::None, EPCGExDistance::None>>();
	}
}

TSharedPtr<PCGExDetails::FDistances> FPCGExDistanceDetails::MakeDistances() const
{
	return PCGExDetails::MakeDistances(Source, Target);
}

bool FPCGExInfluenceDetails::Init(FPCGExContext* InContext, const TSharedRef<PCGExData::FFacade>& InPointDataFacade)
{
	InfluenceBuffer = GetValueSettingInfluence();
	return InfluenceBuffer->Init(InContext, InPointDataFacade, false);
}

bool FPCGExFuseDetailsBase::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InDataFacade)
{
	if (!bComponentWiseTolerance) { Tolerances = FVector(Tolerance); }

	if (!InDataFacade)
	{
		ToleranceGetter = PCGExDetails::MakeSettingValue<FVector>(Tolerances);
	}
	else
	{
		ToleranceGetter = PCGExDetails::MakeSettingValue<FVector>(ToleranceInput, ToleranceAttribute, Tolerances);
	}

	if (!ToleranceGetter->Init(InContext, InDataFacade)) { return false; }

	return true;
}

bool FPCGExFuseDetails::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InDataFacade)
{
	if (!FPCGExFuseDetailsBase::Init(InContext, InDataFacade)) { return false; }

	DistanceDetails = PCGExDetails::MakeDistances(SourceDistance, TargetDistance);

	return true;
}

bool FPCGExManhattanDetails::IsValid() const
{
	return bInitialized;
}

bool FPCGExManhattanDetails::Init(FPCGExContext* InContext, const TSharedPtr<PCGExData::FFacade>& InDataFacade)
{
	if (bSupportAttribute)
	{
		GridSizeBuffer = GetValueSettingGridSize();
		if (!GridSizeBuffer->Init(InContext, InDataFacade)) { return false; }

		if (SpaceAlign == EPCGExManhattanAlign::Custom) { OrientBuffer = GetValueSettingOrient(); }
		else if (SpaceAlign == EPCGExManhattanAlign::World) { OrientBuffer = PCGExDetails::MakeSettingValue(FQuat::Identity); }

		if (OrientBuffer && !OrientBuffer->Init(InContext, InDataFacade)) { return false; }
	}
	else
	{
		GridSize = PCGExMath::Abs(GridSize);
		//GridIntSize = FIntVector3(FMath::Floor(GridSize.X), FMath::Floor(GridSize.Y), FMath::Floor(GridSize.Z));
		GridSizeBuffer = PCGExDetails::MakeSettingValue(GridSize);
		if (SpaceAlign == EPCGExManhattanAlign::Custom) { OrientBuffer = PCGExDetails::MakeSettingValue(OrientConstant); }
		else if (SpaceAlign == EPCGExManhattanAlign::World) { OrientBuffer = PCGExDetails::MakeSettingValue(FQuat::Identity); }
	}

	PCGEx::GetAxisOrder(Order, Comps);

	bInitialized = true;
	return true;
}

int32 FPCGExManhattanDetails::ComputeSubdivisions(const FVector& A, const FVector& B, const int32 Index, TArray<FVector>& OutSubdivisions, double& OutDist) const
{
	FVector DirectionAndSize = B - A;
	const int32 StartIndex = OutSubdivisions.Num();

	FQuat Rotation = FQuat::Identity;

	switch (SpaceAlign)
	{
	case EPCGExManhattanAlign::World:
	case EPCGExManhattanAlign::Custom:
		Rotation = OrientBuffer->Read(Index);
		break;
	case EPCGExManhattanAlign::SegmentX:
		Rotation = FRotationMatrix::MakeFromX(DirectionAndSize).ToQuat();
		break;
	case EPCGExManhattanAlign::SegmentY:
		Rotation = FRotationMatrix::MakeFromY(DirectionAndSize).ToQuat();
		break;
	case EPCGExManhattanAlign::SegmentZ:
		Rotation = FRotationMatrix::MakeFromZ(DirectionAndSize).ToQuat();
		break;
	}

	DirectionAndSize = Rotation.RotateVector(DirectionAndSize);

	if (Method == EPCGExManhattanMethod::Simple)
	{
		OutSubdivisions.Reserve(OutSubdivisions.Num() + 3);

		FVector Sub = FVector::ZeroVector;
		for (int i = 0; i < 3; ++i)
		{
			const int32 Axis = Comps[i];
			const double Dist = DirectionAndSize[Axis];

			if (FMath::IsNearlyZero(Dist)) { continue; }

			OutDist += Dist;
			Sub[Axis] = Dist;

			if (Sub == B) { break; }

			OutSubdivisions.Emplace(Sub);
		}
	}
	else
	{
		FVector Subdivs = PCGExMath::Abs(GridSizeBuffer->Read(Index));
		FVector Maxes = PCGExMath::Abs(DirectionAndSize);
		if (Method == EPCGExManhattanMethod::GridCount)
		{
			Subdivs = FVector(
				FMath::Floor(Maxes.X / Subdivs.X),
				FMath::Floor(Maxes.Y / Subdivs.Y),
				FMath::Floor(Maxes.Z / Subdivs.Z));
		}

		const FVector StepSize = FVector::Min(Subdivs, Maxes);
		const FVector Sign = FVector(FMath::Sign(DirectionAndSize.X), FMath::Sign(DirectionAndSize.Y), FMath::Sign(DirectionAndSize.Z));

		FVector Sub = FVector::ZeroVector;

		bool bAdvance = true;
		while (bAdvance)
		{
			double DistBefore = OutDist;
			for (int i = 0; i < 3; ++i)
			{
				const int32 Axis = Comps[i];
				double Dist = StepSize[Axis];

				if (const double SubAbs = FMath::Abs(Sub[Axis]); SubAbs + Dist > Maxes[Axis]) { Dist = Maxes[Axis] - SubAbs; }
				if (FMath::IsNearlyZero(Dist)) { continue; }

				OutDist += Dist;
				Sub[Axis] += Dist * Sign[Axis];

				if (Sub == B)
				{
					bAdvance = false;
					break;
				}

				OutSubdivisions.Emplace(Sub);
			}

			if (DistBefore == OutDist) { bAdvance = false; }
		}
	}

	for (int i = StartIndex; i < OutSubdivisions.Num(); i++) { OutSubdivisions[i] = A + Rotation.UnrotateVector(OutSubdivisions[i]); }

	return OutSubdivisions.Num() - StartIndex;
}
