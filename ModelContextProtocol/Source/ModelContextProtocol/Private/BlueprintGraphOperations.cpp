#include "BlueprintGraphOperations.h"

#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/UnrealType.h"

namespace
{
    TSharedRef<FJsonObject> Failure(const FString& Message)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), false);
        Result->SetStringField(TEXT("error"), Message);
        return Result;
    }

    FString GuidString(const FGuid& Guid)
    {
        return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
    }

    FString ContainerName(EPinContainerType Type)
    {
        switch (Type)
        {
        case EPinContainerType::Array: return TEXT("array");
        case EPinContainerType::Set: return TEXT("set");
        case EPinContainerType::Map: return TEXT("map");
        default: return TEXT("none");
        }
    }

    TSharedRef<FJsonObject> PinJson(const UEdGraphPin* Pin)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("id"), GuidString(Pin->PinId));
        Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
        Result->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
        int32 LogicalIndex = 0;
        for (const UEdGraphPin* Candidate : Pin->GetOwningNode()->Pins)
        {
            if (!Candidate || Candidate->bHidden || Candidate->Direction != Pin->Direction)
            {
                continue;
            }
            if (Candidate == Pin)
            {
                Result->SetNumberField(TEXT("index"), LogicalIndex);
                break;
            }
            ++LogicalIndex;
        }
        Result->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        Result->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
        Result->SetStringField(TEXT("container"), ContainerName(Pin->PinType.ContainerType));
        if (const UObject* TypeObject = Pin->PinType.PinSubCategoryObject.Get())
        {
            Result->SetStringField(TEXT("subcategory_object"), TypeObject->GetPathName());
        }
        Result->SetStringField(TEXT("default_value"), Pin->DefaultValue);
        Result->SetBoolField(TEXT("hidden"), Pin->bHidden);
        TArray<TSharedPtr<FJsonValue>> Links;
        for (const UEdGraphPin* Linked : Pin->LinkedTo)
        {
            if (!Linked || !Linked->GetOwningNode())
            {
                continue;
            }
            TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
            Link->SetStringField(TEXT("node_id"), GuidString(Linked->GetOwningNode()->NodeGuid));
            Link->SetStringField(TEXT("pin_id"), GuidString(Linked->PinId));
            Link->SetStringField(TEXT("pin_name"), Linked->PinName.ToString());
            Links.Add(MakeShared<FJsonValueObject>(Link));
        }
        Result->SetArrayField(TEXT("links"), MoveTemp(Links));
        return Result;
    }

    TSharedRef<FJsonObject> NodeJson(const UEdGraphNode* Node)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("id"), GuidString(Node->NodeGuid));
        Result->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
        Result->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        Result->SetNumberField(TEXT("x"), Node->NodePosX);
        Result->SetNumberField(TEXT("y"), Node->NodePosY);
        Result->SetStringField(TEXT("comment"), Node->NodeComment);
        if (Node->bHasCompilerMessage)
        {
            Result->SetStringField(TEXT("compiler_message"), Node->ErrorMsg);
            Result->SetNumberField(TEXT("compiler_message_type"), Node->ErrorType);
        }
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (const UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin)
            {
                Pins.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
            }
        }
        Result->SetArrayField(TEXT("pins"), MoveTemp(Pins));
        return Result;
    }

    UBlueprint* LoadBlueprint(const FString& RequestedPath, FString& OutError)
    {
        if (RequestedPath.IsEmpty())
        {
            OutError = TEXT("blueprint_path is required");
            return nullptr;
        }
        FString ObjectPath = RequestedPath;
        if (!ObjectPath.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(ObjectPath))
        {
            ObjectPath += TEXT(".") + FPackageName::GetLongPackageAssetName(ObjectPath);
        }
        UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        if (!Blueprint)
        {
            OutError = FString::Printf(TEXT("Blueprint asset could not be loaded: %s"), *RequestedPath);
        }
        return Blueprint;
    }

    UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& RequestedName, FString& OutError)
    {
        if (RequestedName.IsEmpty())
        {
            if (UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint))
            {
                return EventGraph;
            }
        }
        TArray<UEdGraph*> Graphs;
        Blueprint->GetAllGraphs(Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (Graph && Graph->GetName().Equals(RequestedName, ESearchCase::IgnoreCase))
            {
                return Graph;
            }
        }
        OutError = RequestedName.IsEmpty()
            ? TEXT("Blueprint has no Event Graph")
            : FString::Printf(TEXT("Blueprint graph was not found: %s"), *RequestedName);
        return nullptr;
    }

    UEdGraphNode* FindNode(UEdGraph* Graph, const FString& Id, FString& OutError)
    {
        FGuid Guid;
        if (!FGuid::Parse(Id, Guid))
        {
            OutError = TEXT("node_id must be a valid node GUID returned by inspect");
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node && Node->NodeGuid == Guid)
            {
                return Node;
            }
        }
        OutError = FString::Printf(TEXT("Node was not found in graph: %s"), *Id);
        return nullptr;
    }

    UEdGraphPin* FindPin(UEdGraph* Graph, const FString& Id, FString& OutError)
    {
        FGuid Guid;
        if (!FGuid::Parse(Id, Guid))
        {
            OutError = TEXT("pin_id must be a valid pin GUID returned by inspect");
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Node)
            {
                if (UEdGraphPin* Pin = Node->FindPinById(Guid))
                {
                    return Pin;
                }
            }
        }
        OutError = FString::Printf(TEXT("Pin was not found in graph: %s"), *Id);
        return nullptr;
    }

    UEdGraphPin* FindPinByReference(
        UEdGraph* Graph,
        const FJsonObject& Command,
        const FString& Prefix,
        FString& OutError)
    {
        FString PinId;
        Command.TryGetStringField(Prefix + TEXT("pin_id"), PinId);
        if (!PinId.IsEmpty())
        {
            return FindPin(Graph, PinId, OutError);
        }

        FString NodeId;
        FString PinName;
        FString Direction;
        int32 RequestedIndex = 0;
        Command.TryGetStringField(Prefix + TEXT("node_id"), NodeId);
        Command.TryGetStringField(Prefix + TEXT("pin_name"), PinName);
        Command.TryGetStringField(Prefix + TEXT("pin_direction"), Direction);
        Command.TryGetNumberField(Prefix + TEXT("pin_index"), RequestedIndex);
        UEdGraphNode* Node = FindNode(Graph, NodeId, OutError);
        if (!Node)
        {
            return nullptr;
        }
        if (PinName.IsEmpty() || (Direction != TEXT("input") && Direction != TEXT("output")))
        {
            OutError = Prefix + TEXT("pin_id, or node_id + pin_name + pin_direction, is required");
            return nullptr;
        }
        const EEdGraphPinDirection PinDirection = Direction == TEXT("input") ? EGPD_Input : EGPD_Output;
        TArray<UEdGraphPin*, TInlineAllocator<16>> Candidates;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && !Pin->bHidden && Pin->Direction == PinDirection)
            {
                Candidates.Add(Pin);
            }
        }
        if (Candidates.IsValidIndex(RequestedIndex)
            && Candidates[RequestedIndex]->PinName.ToString() == PinName)
        {
            return Candidates[RequestedIndex];
        }
        for (int32 Distance = 1; Distance < Candidates.Num(); ++Distance)
        {
            const int32 Forward = RequestedIndex + Distance;
            if (Candidates.IsValidIndex(Forward) && Candidates[Forward]->PinName.ToString() == PinName)
            {
                return Candidates[Forward];
            }
            const int32 Backward = RequestedIndex - Distance;
            if (Candidates.IsValidIndex(Backward) && Candidates[Backward]->PinName.ToString() == PinName)
            {
                return Candidates[Backward];
            }
        }
        OutError = FString::Printf(TEXT("Pin reference no longer resolves on node %s: %s"), *NodeId, *PinName);
        return nullptr;
    }

    void GetPosition(const FJsonObject& Command, int32& X, int32& Y)
    {
        X = 0;
        Y = 0;
        Command.TryGetNumberField(TEXT("x"), X);
        Command.TryGetNumberField(TEXT("y"), Y);
    }

    void MarkChanged(UBlueprint* Blueprint, UEdGraph* Graph, bool bStructural = false)
    {
        Graph->NotifyGraphChanged();
        if (bStructural)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        Blueprint->MarkPackageDirty();
    }

    void MarkSpawned(UBlueprint* Blueprint, UK2Node* Node)
    {
        if (Node->GetSchema() && Node->GetSchema()->MarkBlueprintDirtyFromNewNode(Blueprint, Node))
        {
            return;
        }
        if (Node->NodeCausesStructuralBlueprintChange())
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        }
        else
        {
            FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
        }
        Blueprint->MarkPackageDirty();
    }

    TSharedRef<FJsonObject> NodeResult(UEdGraphNode* Node, bool bExisting = false)
    {
        TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetBoolField(TEXT("ok"), true);
        Result->SetBoolField(TEXT("existing"), bExisting);
        Result->SetObjectField(TEXT("node"), NodeJson(Node));
        return Result;
    }

    UFunction* ResolveFunction(UBlueprint* Blueprint, const FJsonObject& Command, FString& OutError)
    {
        FString FunctionPath;
        FString FunctionClass;
        FString FunctionName;
        Command.TryGetStringField(TEXT("function_path"), FunctionPath);
        Command.TryGetStringField(TEXT("function_class"), FunctionClass);
        Command.TryGetStringField(TEXT("function_name"), FunctionName);

        if (!FunctionPath.IsEmpty())
        {
            TSoftObjectPtr<UFunction> FunctionPointer{FSoftObjectPath(FunctionPath)};
            if (UFunction* Function = FunctionPointer.LoadSynchronous())
            {
                return Function;
            }
            if (Blueprint->SkeletonGeneratedClass)
            {
                if (UFunction* Function = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionPath)))
                {
                    return Function;
                }
            }
        }

        UClass* OwnerClass = nullptr;
        if (!FunctionClass.IsEmpty())
        {
            OwnerClass = LoadObject<UClass>(nullptr, *FunctionClass);
            if (!OwnerClass)
            {
                OutError = FString::Printf(TEXT("Function owner class could not be loaded: %s"), *FunctionClass);
                return nullptr;
            }
        }
        else
        {
            OwnerClass = Blueprint->SkeletonGeneratedClass
                ? Blueprint->SkeletonGeneratedClass
                : Blueprint->GeneratedClass;
        }
        if (!FunctionName.IsEmpty() && OwnerClass)
        {
            if (UFunction* Function = OwnerClass->FindFunctionByName(FName(*FunctionName)))
            {
                return Function;
            }
        }
        OutError = FunctionPath.IsEmpty() && FunctionName.IsEmpty()
            ? TEXT("function_path or function_name is required")
            : FString::Printf(TEXT("UFunction could not be resolved: %s%s"), *FunctionPath, *FunctionName);
        return nullptr;
    }
}

namespace UnrealMCPBlueprintGraph
{
    TSharedRef<FJsonObject> Execute(
        const TSharedRef<FJsonObject>& Command,
        bool& bOutSucceeded,
        bool& bOutSideEffectsPossible)
    {
        bOutSucceeded = false;
        bOutSideEffectsPossible = false;
        FString Operation;
        FString BlueprintPath;
        FString GraphName;
        Command->TryGetStringField(TEXT("operation"), Operation);
        Command->TryGetStringField(TEXT("blueprint_path"), BlueprintPath);
        Command->TryGetStringField(TEXT("graph"), GraphName);

        FString Error;
        UBlueprint* Blueprint = LoadBlueprint(BlueprintPath, Error);
        if (!Blueprint)
        {
            return Failure(Error);
        }
        UEdGraph* Graph = FindGraph(Blueprint, GraphName, Error);
        if (!Graph)
        {
            return Failure(Error);
        }

        if (Operation == TEXT("inspect"))
        {
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
            Result->SetStringField(TEXT("graph"), Graph->GetName());
            Result->SetStringField(TEXT("graph_id"), GuidString(Graph->GraphGuid));
            TArray<TSharedPtr<FJsonValue>> Nodes;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node)
                {
                    Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node)));
                }
            }
            Result->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
            bOutSucceeded = true;
            return Result;
        }

        bOutSideEffectsPossible = true;
        Blueprint->Modify();
        Graph->Modify();

        if (Operation == TEXT("add_event"))
        {
            if (!FBlueprintEditorUtils::IsEventGraph(Graph))
            {
                return Failure(TEXT("add_event requires an Event Graph"));
            }
            FString EventName;
            FString EventClassPath;
            Command->TryGetStringField(TEXT("event_name"), EventName);
            Command->TryGetStringField(TEXT("event_class"), EventClassPath);
            if (EventName.IsEmpty())
            {
                return Failure(TEXT("event_name is required"));
            }
            for (UEdGraphNode* ExistingNode : Graph->Nodes)
            {
                if (UK2Node_Event* ExistingEvent = Cast<UK2Node_Event>(ExistingNode))
                {
                    if (!Cast<UK2Node_CustomEvent>(ExistingEvent)
                        && ExistingEvent->GetFunctionName().ToString().Equals(EventName, ESearchCase::IgnoreCase))
                    {
                        bOutSucceeded = true;
                        return NodeResult(ExistingEvent, true);
                    }
                }
            }
            UClass* EventClass = EventClassPath.IsEmpty()
                ? Blueprint->ParentClass.Get()
                : LoadObject<UClass>(nullptr, *EventClassPath);
            if (!EventClass || !EventClass->FindFunctionByName(FName(*EventName)))
            {
                return Failure(FString::Printf(TEXT("Blueprint event function was not found on class: %s"), *EventName));
            }
            int32 X;
            int32 Y;
            GetPosition(*Command, X, Y);
            UK2Node_Event* Node = FKismetEditorUtilities::AddDefaultEventNode(
                Blueprint, Graph, FName(*EventName), EventClass, Y);
            if (!Node)
            {
                return Failure(TEXT("UE rejected the event node (hidden, disallowed, or duplicate)"));
            }
            Node->NodePosX = X;
            Node->NodePosY = Y;
            Node->SetEnabledState(ENodeEnabledState::Enabled);
            MarkChanged(Blueprint, Graph, true);
            bOutSucceeded = true;
            return NodeResult(Node);
        }

        if (Operation == TEXT("add_custom_event"))
        {
            if (!FBlueprintEditorUtils::IsEventGraph(Graph))
            {
                return Failure(TEXT("add_custom_event requires an Event Graph"));
            }
            FString EventName;
            Command->TryGetStringField(TEXT("event_name"), EventName);
            if (EventName.IsEmpty())
            {
                return Failure(TEXT("event_name is required"));
            }
            for (UEdGraphNode* ExistingNode : Graph->Nodes)
            {
                if (UK2Node_CustomEvent* ExistingEvent = Cast<UK2Node_CustomEvent>(ExistingNode))
                {
                    if (ExistingEvent->CustomFunctionName.ToString().Equals(EventName, ESearchCase::IgnoreCase))
                    {
                        bOutSucceeded = true;
                        return NodeResult(ExistingEvent, true);
                    }
                }
            }
            int32 X;
            int32 Y;
            GetPosition(*Command, X, Y);
            FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
            UK2Node_CustomEvent* Node = Creator.CreateNode(false);
            Node->CustomFunctionName = FName(*EventName);
            Node->NodePosX = X;
            Node->NodePosY = Y;
            Creator.Finalize();
            MarkChanged(Blueprint, Graph, true);
            bOutSucceeded = true;
            return NodeResult(Node);
        }

        if (Operation == TEXT("add_function_call"))
        {
            UFunction* Function = ResolveFunction(Blueprint, *Command, Error);
            if (!Function)
            {
                return Failure(Error);
            }
            int32 X;
            int32 Y;
            GetPosition(*Command, X, Y);
            UBlueprintFunctionNodeSpawner* Spawner = UBlueprintFunctionNodeSpawner::Create(Function);
            UK2Node_CallFunction* Node = Spawner
                ? Cast<UK2Node_CallFunction>(Spawner->Invoke(Graph, {}, FVector2D(X, Y)))
                : nullptr;
            if (!Node)
            {
                return Failure(TEXT("Blueprint function node spawner rejected the function in this graph"));
            }
            MarkSpawned(Blueprint, Node);
            bOutSucceeded = true;
            return NodeResult(Node);
        }

        if (Operation == TEXT("add_variable_get") || Operation == TEXT("add_variable_set"))
        {
            FString VariableName;
            Command->TryGetStringField(TEXT("variable_name"), VariableName);
            UClass* Scope = Blueprint->SkeletonGeneratedClass
                ? Blueprint->SkeletonGeneratedClass
                : Blueprint->GeneratedClass;
            FProperty* Property = Scope ? FindFProperty<FProperty>(Scope, FName(*VariableName)) : nullptr;
            if (!Property)
            {
                return Failure(FString::Printf(TEXT("Blueprint variable was not found: %s"), *VariableName));
            }
            int32 X;
            int32 Y;
            GetPosition(*Command, X, Y);
            UBlueprintVariableNodeSpawner* Spawner = UBlueprintVariableNodeSpawner::CreateFromMemberOrParam(
                Operation == TEXT("add_variable_get")
                    ? UK2Node_VariableGet::StaticClass()
                    : UK2Node_VariableSet::StaticClass(),
                Property);
            UK2Node* ResultNode = Spawner
                ? Cast<UK2Node>(Spawner->Invoke(Graph, {}, FVector2D(X, Y)))
                : nullptr;
            if (!ResultNode)
            {
                return Failure(TEXT("Blueprint variable node spawner rejected the variable in this graph"));
            }
            MarkSpawned(Blueprint, ResultNode);
            bOutSucceeded = true;
            return NodeResult(ResultNode);
        }

        if (Operation == TEXT("connect"))
        {
            UEdGraphPin* FromPin = FindPinByReference(Graph, *Command, TEXT("from_"), Error);
            if (!FromPin) return Failure(Error);
            UEdGraphPin* ToPin = FindPinByReference(Graph, *Command, TEXT("to_"), Error);
            if (!ToPin) return Failure(Error);
            if (FromPin->LinkedTo.Contains(ToPin))
            {
                TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), true);
                Result->SetBoolField(TEXT("existing"), true);
                bOutSucceeded = true;
                return Result;
            }
            const UEdGraphSchema* Schema = Graph->GetSchema();
            if (!Schema || !Schema->TryCreateConnection(FromPin, ToPin))
            {
                return Failure(TEXT("K2 schema rejected the pin connection; inspect pin directions and types"));
            }
            MarkChanged(Blueprint, Graph);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            bOutSucceeded = true;
            return Result;
        }

        if (Operation == TEXT("disconnect"))
        {
            FString ToPinId;
            FString ToNodeId;
            Command->TryGetStringField(TEXT("to_pin_id"), ToPinId);
            Command->TryGetStringField(TEXT("to_node_id"), ToNodeId);
            UEdGraphPin* FromPin = FindPinByReference(Graph, *Command, TEXT("from_"), Error);
            if (!FromPin) return Failure(Error);
            const UEdGraphSchema* Schema = Graph->GetSchema();
            if (!Schema) return Failure(TEXT("Graph has no schema"));
            if (ToPinId.IsEmpty() && ToNodeId.IsEmpty())
            {
                Schema->BreakPinLinks(*FromPin, true);
            }
            else
            {
                UEdGraphPin* ToPin = FindPinByReference(Graph, *Command, TEXT("to_"), Error);
                if (!ToPin) return Failure(Error);
                Schema->BreakSinglePinLink(FromPin, ToPin);
            }
            MarkChanged(Blueprint, Graph);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            bOutSucceeded = true;
            return Result;
        }

        if (Operation == TEXT("set_pin_default"))
        {
            FString DefaultValue;
            Command->TryGetStringField(TEXT("default_value"), DefaultValue);
            UEdGraphPin* Pin = FindPinByReference(Graph, *Command, FString(), Error);
            if (!Pin) return Failure(Error);
            const UEdGraphSchema* Schema = Graph->GetSchema();
            if (!Schema) return Failure(TEXT("Graph has no schema"));
            const FString Validation = Schema->IsPinDefaultValid(Pin, DefaultValue, nullptr, FText::GetEmpty());
            if (!Validation.IsEmpty())
            {
                return Failure(FString::Printf(TEXT("Invalid pin default: %s"), *Validation));
            }
            Schema->TrySetDefaultValue(*Pin, DefaultValue);
            MarkChanged(Blueprint, Graph);
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), true);
            Result->SetObjectField(TEXT("pin"), PinJson(Pin));
            bOutSucceeded = true;
            return Result;
        }

        if (Operation == TEXT("remove_node") || Operation == TEXT("move_node") || Operation == TEXT("set_comment"))
        {
            FString NodeId;
            Command->TryGetStringField(TEXT("node_id"), NodeId);
            UEdGraphNode* Node = FindNode(Graph, NodeId, Error);
            if (!Node) return Failure(Error);
            Node->Modify();
            if (Operation == TEXT("remove_node"))
            {
                if (!Node->CanUserDeleteNode())
                {
                    return Failure(TEXT("UE does not allow this node to be deleted by a user action"));
                }
                const UK2Node* K2Node = Cast<UK2Node>(Node);
                const bool bStructural = K2Node && K2Node->NodeCausesStructuralBlueprintChange();
                FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
                MarkChanged(Blueprint, Graph, bStructural);
                TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
                Result->SetBoolField(TEXT("ok"), true);
                Result->SetStringField(TEXT("removed_node_id"), NodeId);
                bOutSucceeded = true;
                return Result;
            }
            if (Operation == TEXT("move_node"))
            {
                int32 X;
                int32 Y;
                GetPosition(*Command, X, Y);
                Node->NodePosX = X;
                Node->NodePosY = Y;
            }
            else
            {
                FString Comment;
                Command->TryGetStringField(TEXT("comment"), Comment);
                Node->NodeComment = Comment;
            }
            MarkChanged(Blueprint, Graph);
            bOutSucceeded = true;
            return NodeResult(Node);
        }

        if (Operation == TEXT("compile"))
        {
            FCompilerResultsLog CompilerLog;
            CompilerLog.bSilentMode = true;
            CompilerLog.bAnnotateMentionedNodes = true;
            FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
            bool bSave = false;
            Command->TryGetBoolField(TEXT("save"), bSave);
            bool bSaved = false;
            if (bSave && GEditor)
            {
                if (UEditorAssetSubsystem* Assets = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
                {
                    bSaved = Assets->SaveLoadedAsset(Blueprint, false);
                }
            }
            TArray<TSharedPtr<FJsonValue>> NodeMessages;
            for (const UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->bHasCompilerMessage)
                {
                    NodeMessages.Add(MakeShared<FJsonValueObject>(NodeJson(Node)));
                }
            }
            TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
            Result->SetBoolField(TEXT("ok"), CompilerLog.NumErrors == 0 && (!bSave || bSaved));
            Result->SetNumberField(TEXT("errors"), CompilerLog.NumErrors);
            Result->SetNumberField(TEXT("warnings"), CompilerLog.NumWarnings);
            Result->SetBoolField(TEXT("save_requested"), bSave);
            Result->SetBoolField(TEXT("saved"), bSaved);
            Result->SetArrayField(TEXT("node_messages"), MoveTemp(NodeMessages));
            if (CompilerLog.NumErrors > 0)
            {
                Result->SetStringField(TEXT("error"), TEXT("Blueprint compilation failed; inspect node_messages"));
            }
            else if (bSave && !bSaved)
            {
                Result->SetStringField(TEXT("error"), TEXT("Blueprint compiled but could not be saved"));
            }
            bOutSucceeded = CompilerLog.NumErrors == 0 && (!bSave || bSaved);
            return Result;
        }

        return Failure(FString::Printf(TEXT("Unsupported blueprint_graph operation: %s"), *Operation));
    }
}
