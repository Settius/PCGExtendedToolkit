﻿// Copyright 2025 Timothé Lapetite and contributors
// Released under the MIT license https://opensource.org/license/MIT/

#include "Graph/PCGExEdgesProcessor.h"
#include "Graph/PCGExClusterMT.h"

#define LOCTEXT_NAMESPACE "PCGExGraphSettings"

#pragma region UPCGSettings interface


PCGExData::EIOInit UPCGExEdgesProcessorSettings::GetMainOutputInitMode() const { return PCGExData::EIOInit::Forward; }
PCGExData::EIOInit UPCGExEdgesProcessorSettings::GetEdgeOutputInitMode() const { return PCGExData::EIOInit::Forward; }

bool UPCGExEdgesProcessorSettings::GetMainAcceptMultipleData() const { return true; }

bool UPCGExEdgesProcessorSettings::WantsScopedIndexLookupBuild() const
{
	PCGEX_GET_OPTION_STATE(ScopedIndexLookupBuild, bDefaultScopedIndexLookupBuild)
}

FPCGExEdgesProcessorContext::~FPCGExEdgesProcessorContext()
{
	PCGEX_TERMINATE_ASYNC

	for (TSharedPtr<PCGExClusterMT::IBatch>& Batch : Batches)
	{
		Batch->Cleanup();
		Batch.Reset();
	}

	Batches.Empty();
}

TArray<FPCGPinProperties> UPCGExEdgesProcessorSettings::InputPinProperties() const
{
	//TArray<FPCGPinProperties> PinProperties = Super::InputPinProperties();

	TArray<FPCGPinProperties> PinProperties;

	if (!IsInputless())
	{
		if (GetMainAcceptMultipleData()) { PCGEX_PIN_POINTS(GetMainInputPin(), "The point data to be processed.", Required, {}) }
		else { PCGEX_PIN_POINT(GetMainInputPin(), "The point data to be processed.", Required, {}) }
	}

	PCGEX_PIN_POINTS(PCGExGraph::SourceEdgesLabel, "Edges associated with the main input points", Required, {})

	if (SupportsPointFilters())
	{
		if (RequiresPointFilters()) { PCGEX_PIN_FACTORIES(GetPointFilterPin(), GetPointFilterTooltip(), Required, {}) }
		else { PCGEX_PIN_FACTORIES(GetPointFilterPin(), GetPointFilterTooltip(), Normal, {}) }
	}

	if (SupportsEdgeSorting())
	{
		if (RequiresEdgeSorting()) { PCGEX_PIN_FACTORIES(PCGExGraph::SourceEdgeSortingRules, "Plug sorting rules here. Order is defined by each rule' priority value, in ascending order.", Required, {}) }
		else { PCGEX_PIN_FACTORIES(PCGExGraph::SourceEdgeSortingRules, "Plug sorting rules here. Order is defined by each rule' priority value, in ascending order.", Normal, {}) }
	}
	return PinProperties;
}

TArray<FPCGPinProperties> UPCGExEdgesProcessorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> PinProperties = Super::OutputPinProperties();
	PCGEX_PIN_POINTS(PCGExGraph::OutputEdgesLabel, "Edges associated with the main output points", Required, {})
	return PinProperties;
}

bool UPCGExEdgesProcessorSettings::SupportsEdgeSorting() const { return false; }

bool UPCGExEdgesProcessorSettings::RequiresEdgeSorting() const { return true; }

#pragma endregion

const TArray<FPCGExSortRuleConfig>* FPCGExEdgesProcessorContext::GetEdgeSortingRules() const
{
	if (EdgeSortingRules.IsEmpty()) { return nullptr; }
	return &EdgeSortingRules;
}

bool FPCGExEdgesProcessorContext::AdvancePointsIO(const bool bCleanupKeys)
{
	CurrentCluster.Reset();

	CurrentEdgesIndex = -1;

	if (!FPCGExPointsProcessorContext::AdvancePointsIO(bCleanupKeys)) { return false; }

	if (const PCGExData::DataIDType TagValue = CurrentIO->Tags->GetTypedValue<int32>(PCGExGraph::TagStr_PCGExCluster))
	{
		int32 PreUpdateKey = TagValue->Value;
		TaggedEdges = InputDictionary->GetEntries(PreUpdateKey);

		if (TaggedEdges && !TaggedEdges->Entries.IsEmpty())
		{
			PCGExData::DataIDType OutId;
			PCGExGraph::SetClusterVtx(CurrentIO, OutId); // Update key
			PCGExGraph::MarkClusterEdges(TaggedEdges->Entries, OutId);
		}
		else { TaggedEdges = nullptr; }
	}
	else { TaggedEdges = nullptr; }

	if (!TaggedEdges && !bQuietMissingClusterPairElement)
	{
		PCGE_LOG_C(Warning, GraphAndLog, this, FTEXT("Some input vtx have no associated edges."));
	}

	return true;
}

void FPCGExEdgesProcessorContext::OutputBatches() const
{
	for (const TSharedPtr<PCGExClusterMT::IBatch>& Batch : Batches) { Batch->Output(); }
}

bool FPCGExEdgesProcessorContext::ProcessClusters(const PCGEx::ContextState NextStateId, const bool bIsNextStateAsync)
{
	//FWriteScopeLock WriteScopeLock(ClusterProcessingLock); // Just in case

	if (!bBatchProcessingEnabled) { return true; }

	if (bClusterBatchInlined)
	{
		if (!CurrentBatch)
		{
			if (CurrentBatchIndex == -1)
			{
				// First batch
				AdvanceBatch(NextStateId, bIsNextStateAsync);
				return false;
			}

			return true;
		}

		PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExClusterMT::MTState_ClusterProcessing)
		{
			if (!CurrentBatch->bSkipCompletion)
			{
				SetAsyncState(PCGExClusterMT::MTState_ClusterCompletingWork);
				CurrentBatch->CompleteWork();
			}
			else
			{
				SetState(PCGExClusterMT::MTState_ClusterCompletingWork);
			}
		}

		PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExClusterMT::MTState_ClusterCompletingWork)
		{
			AdvanceBatch(NextStateId, bIsNextStateAsync);
		}
	}
	else
	{
		PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExClusterMT::MTState_ClusterProcessing)
		{
			ClusterProcessing_InitialProcessingDone();

			if (!bSkipClusterBatchCompletionStep)
			{
				SetAsyncState(PCGExClusterMT::MTState_ClusterCompletingWork);
				CompleteBatches(Batches);
			}
			else
			{
				SetState(PCGExClusterMT::MTState_ClusterCompletingWork);
			}
		}

		PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExClusterMT::MTState_ClusterCompletingWork)
		{
			if (!bSkipClusterBatchCompletionStep) { ClusterProcessing_WorkComplete(); }

			if (bDoClusterBatchWritingStep)
			{
				SetAsyncState(PCGExClusterMT::MTState_ClusterWriting);
				WriteBatches(Batches);
				return false;
			}

			bBatchProcessingEnabled = false;
			if (NextStateId == PCGEx::State_Done) { Done(); }
			if (bIsNextStateAsync) { SetAsyncState(NextStateId); }
			else { SetState(NextStateId); }
		}

		PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExClusterMT::MTState_ClusterWriting)
		{
			ClusterProcessing_WritingDone();

			bBatchProcessingEnabled = false;
			if (NextStateId == PCGEx::State_Done) { Done(); }
			if (bIsNextStateAsync) { SetAsyncState(NextStateId); }
			else { SetState(NextStateId); }
		}
	}

	return false;
}

bool FPCGExEdgesProcessorContext::CompileGraphBuilders(const bool bOutputToContext, const PCGEx::ContextState NextStateId)
{
	PCGEX_ON_STATE_INTERNAL(PCGExGraph::State_ReadyToCompile)
	{
		SetAsyncState(PCGExGraph::State_Compiling);
		for (const TSharedPtr<PCGExClusterMT::IBatch>& Batch : Batches) { Batch->CompileGraphBuilder(bOutputToContext); }
		return false;
	}

	PCGEX_ON_ASYNC_STATE_READY_INTERNAL(PCGExGraph::State_Compiling)
	{
		ClusterProcessing_GraphCompilationDone();
		SetState(NextStateId);
	}

	return true;
}

void FPCGExEdgesProcessorContext::ClusterProcessing_InitialProcessingDone()
{
}

void FPCGExEdgesProcessorContext::ClusterProcessing_WorkComplete()
{
}

void FPCGExEdgesProcessorContext::ClusterProcessing_WritingDone()
{
}

void FPCGExEdgesProcessorContext::ClusterProcessing_GraphCompilationDone()
{
}

void FPCGExEdgesProcessorContext::AdvanceBatch(const PCGEx::ContextState NextStateId, const bool bIsNextStateAsync)
{
	CurrentBatchIndex++;
	if (!Batches.IsValidIndex(CurrentBatchIndex))
	{
		CurrentBatch = nullptr;
		bBatchProcessingEnabled = false;
		if (NextStateId == PCGEx::State_Done) { Done(); }
		if (bIsNextStateAsync) { SetAsyncState(NextStateId); }
		else { SetState(NextStateId); }
	}
	else
	{
		CurrentBatch = Batches[CurrentBatchIndex];
		ScheduleBatch(GetAsyncManager(), CurrentBatch, bScopedIndexLookupBuild);
		SetAsyncState(PCGExClusterMT::MTState_ClusterProcessing);
	}
}

void FPCGExEdgesProcessorContext::OutputPointsAndEdges() const
{
	MainPoints->StageOutputs();
	MainEdges->StageOutputs();
}

int32 FPCGExEdgesProcessorContext::GetClusterProcessorsNum() const
{
	int32 Num = 0;
	for (const TSharedPtr<PCGExClusterMT::IBatch>& Batch : Batches) { Num += Batch->GetNumProcessors(); }
	return Num;
}

void FPCGExEdgesProcessorElement::DisabledPassThroughData(FPCGContext* Context) const
{
	FPCGExPointsProcessorElement::DisabledPassThroughData(Context);

	//Forward main edges
	TArray<FPCGTaggedData> EdgesSources = Context->InputData.GetInputsByPin(PCGExGraph::SourceEdgesLabel);
	for (const FPCGTaggedData& TaggedData : EdgesSources)
	{
		FPCGTaggedData& TaggedDataCopy = Context->OutputData.TaggedData.Emplace_GetRef();
		TaggedDataCopy.Data = TaggedData.Data;
		TaggedDataCopy.Tags.Append(TaggedData.Tags);
		TaggedDataCopy.Pin = PCGExGraph::OutputEdgesLabel;
	}
}

bool FPCGExEdgesProcessorElement::Boot(FPCGExContext* InContext) const
{
	if (!FPCGExPointsProcessorElement::Boot(InContext)) { return false; }

	PCGEX_CONTEXT_AND_SETTINGS(EdgesProcessor)

	Context->bQuietMissingClusterPairElement = Settings->bQuietMissingClusterPairElement;

	Context->bHasValidHeuristics = PCGExFactories::GetInputFactories(
		Context, PCGExGraph::SourceHeuristicsLabel, Context->HeuristicsFactories,
		{PCGExFactories::EType::Heuristics}, false);

	Context->InputDictionary = MakeShared<PCGExData::FPointIOTaggedDictionary>(PCGExGraph::TagStr_PCGExCluster);

	TArray<TSharedPtr<PCGExData::FPointIO>> TaggedVtx;
	TArray<TSharedPtr<PCGExData::FPointIO>> TaggedEdges;

	Context->MainEdges = MakeShared<PCGExData::FPointIOCollection>(Context);
	Context->MainEdges->OutputPin = PCGExGraph::OutputEdgesLabel;
	TArray<FPCGTaggedData> Sources = Context->InputData.GetInputsByPin(PCGExGraph::SourceEdgesLabel);
	Context->MainEdges->Initialize(Sources, Settings->GetEdgeOutputInitMode());

	// Gather Vtx inputs
	for (const TSharedPtr<PCGExData::FPointIO>& MainIO : Context->MainPoints->Pairs)
	{
		if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExVtx))
		{
			if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExEdges))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges -- it will be ignored for safety."));
				continue;
			}

			TaggedVtx.Add(MainIO);
			continue;
		}

		if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExEdges))
		{
			if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExVtx))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, some Edge data made its way to the vtx input. It will be ignored."));
			continue;
		}

		PCGE_LOG(Warning, GraphAndLog, FTEXT("A data pluggued into Vtx is neither tagged Vtx or Edges and will be ignored."));
	}

	// Gather Edge inputs
	for (const TSharedPtr<PCGExData::FPointIO>& MainIO : Context->MainEdges->Pairs)
	{
		if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExEdges))
		{
			if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExVtx))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			TaggedEdges.Add(MainIO);
			continue;
		}

		if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExVtx))
		{
			if (MainIO->Tags->IsTagged(PCGExGraph::TagStr_PCGExEdges))
			{
				PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, a data is marked as both Vtx and Edges. It will be ignored."));
				continue;
			}

			PCGE_LOG(Warning, GraphAndLog, FTEXT("Uh oh, some Edge data made its way to the vtx input. It will be ignored."));
			continue;
		}

		PCGE_LOG(Warning, GraphAndLog, FTEXT("A data pluggued into Edges is neither tagged Edges or Vtx and will be ignored."));
	}


	for (const TSharedPtr<PCGExData::FPointIO>& Vtx : TaggedVtx)
	{
		if (!PCGExGraph::IsPointDataVtxReady(Vtx->GetIn()->Metadata))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("A Vtx input has no metadata and will be discarded."));
			Vtx->Disable();
			continue;
		}

		if (!Context->InputDictionary->CreateKey(Vtx.ToSharedRef()))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("At least two Vtx inputs share the same PCGEx/Cluster tag. Only one will be processed."));
			Vtx->Disable();
		}
	}

	for (const TSharedPtr<PCGExData::FPointIO>& Edges : TaggedEdges)
	{
		if (!PCGExGraph::IsPointDataEdgeReady(Edges->GetIn()->Metadata))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("An Edges input has no edge metadata and will be discarded."));
			Edges->Disable();
			continue;
		}

		if (!Context->InputDictionary->TryAddEntry(Edges.ToSharedRef()))
		{
			PCGE_LOG(Warning, GraphAndLog, FTEXT("Some input edges have no associated vtx."));
		}
	}

	if (Context->MainEdges->IsEmpty())
	{
		PCGE_LOG(Error, GraphAndLog, FTEXT("Missing Edges."));
		return false;
	}

	if (Settings->SupportsEdgeSorting())
	{
		Context->EdgeSortingRules = PCGExSorting::GetSortingRules(Context, PCGExGraph::SourceEdgeSortingRules);

		if (Settings->RequiresEdgeSorting() && Context->EdgeSortingRules.IsEmpty())
		{
			PCGE_LOG(Error, GraphAndLog, FTEXT("Missing valid sorting rules."));
			return false;
		}
	}

	return true;
}

void FPCGExEdgesProcessorElement::OnContextInitialized(FPCGExPointsProcessorContext* InContext) const
{
	FPCGExPointsProcessorElement::OnContextInitialized(InContext);

	PCGEX_CONTEXT_AND_SETTINGS(EdgesProcessor)

	Context->bScopedIndexLookupBuild = Settings->WantsScopedIndexLookupBuild();
}

#undef LOCTEXT_NAMESPACE
