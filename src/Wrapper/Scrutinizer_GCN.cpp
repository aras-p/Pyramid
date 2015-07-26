
#include "Scrutinizer_GCN.h"
#include "AMDAsic_Impl.h"
#include "AMDShader_Impl.h"
#include "Utilities.h"

#pragma unmanaged
#include "GCNIsa.h"
#include "GCNDecoder.h"
#include "GCNDisassembler.h"
#include "GCNBufferedPrinter.h"
#include "GCNSimulator.h"
#include <vector>
#include <unordered_map>
#pragma managed


using namespace System::Collections::Generic;
using namespace Pyramid::Scrutinizer;


private ref class GCNInstruction : public IInstruction
{
public:
    GCNInstruction( const GCN::Instruction& inst )
    {
        m_pInstruction = new GCN::Instruction();
        *m_pInstruction = inst;
    }

    virtual ~GCNInstruction() { delete m_pInstruction; }
        
    property BasicBlock^ Block
    {
        virtual BasicBlock^ get() { return m_pmBlock; } 
        virtual void set( BasicBlock^ bl ) { m_pmBlock = bl; }
    }

    virtual System::String^ Disassemble()
    {
        GCN::Disassembler::BufferedPrinter printer;
        GCN::Disassembler::Disassemble( printer, m_pInstruction );
        printer.m_Bytes.push_back(0);

        System::String^ str = MakeString( printer.m_Bytes.data() );
        return str->Replace( "\n", System::Environment::NewLine );
    }
    virtual  System::String^ ToString() override
    {
        return Disassemble();
    }

internal:
    GCN::Instruction* m_pInstruction;
    BasicBlock^ m_pmBlock;
};

    
private ref class GCNJump : public GCNInstruction, public IJumpInstruction
{
public:
    GCNJump( const GCN::Instruction& inst ) : GCNInstruction(inst), m_Target(nullptr) {}
        
    property IInstruction^ Target{
        virtual IInstruction^ get () { return m_Target; }
    };

internal:
    IInstruction^ m_Target;
};

private ref class GCNBranch : public GCNInstruction, public IBranchInstruction
{
public:
    GCNBranch( const GCN::Instruction& inst ) : GCNInstruction(inst), m_IfTarget(nullptr), m_ElseTarget(nullptr){}
        
    property IInstruction^ IfTarget{
        virtual IInstruction^ get () { return m_IfTarget; }
    };
        
    property IInstruction^ ElseTarget{
        virtual IInstruction^ get () { return m_ElseTarget; }
    };

    property BranchCategory Category{
        virtual BranchCategory get() { return m_eCategory; }
        virtual void set( BranchCategory e ) { m_eCategory = e; }
    }

internal:
    IInstruction^ m_IfTarget;
    IInstruction^ m_ElseTarget;
    BranchCategory m_eCategory;
};

private ref class GCNSamplingOp : public GCNInstruction, public ISamplingInstruction
{
public:
    GCNSamplingOp( const GCN::Instruction& inst ) : GCNInstruction(inst)
    {
    }

    property TextureFilter Filter{
        virtual TextureFilter get () { return m_eFilter; }
        virtual void set( TextureFilter e ) { m_eFilter = e; }
    };
       
    property TexelFormat Format{
        virtual TexelFormat get () { return m_eFormat; }
        virtual void set( TexelFormat e ) { m_eFormat = e; }
    };
internal:
    TextureFilter m_eFilter;
    TexelFormat m_eFormat;
};


private ref class GCNTBufferOp : public GCNInstruction, public ITexelLoadInstruction
{
public:
    GCNTBufferOp( const GCN::Instruction& inst ) : GCNInstruction(inst)
    {
    }

    property TexelFormat Format{
        virtual TexelFormat get () { return m_eFormat; }
        virtual void set( TexelFormat e ) { m_eFormat = e; }
    };

internal:
    TexelFormat m_eFormat;
};

   

    
List<IInstruction^>^ Scrutinizer_GCN::BuildProgram(  )
{
    
    MarshalledBlob^ bl = gcnew MarshalledBlob(m_pmShader->ReadISABytes());
    unsigned char* pBytes = bl->GetBlob();
    size_t nBytes = bl->GetLength();

    GCN::IDecoder* pDecoder = m_pmAsic->m_pDecoder;

        
    //
    // First pass:
    //   1.  Validate
    //   2.  Figure out how many instructions there are
    //   3.  Identify branches and their targets
    //
    std::vector< const GCN::uint8* > Branches;
    std::vector< const GCN::uint8* > Jumps;
    std::unordered_map< const void*, size_t > IndexMap;
     
    List<IInstruction^>^ pmManagedInstructions = gcnew List<IInstruction^>();

    size_t nOffs=0;
    while( nOffs < nBytes )
    {
        unsigned char* pLocation = pBytes + nOffs;

        // read instruction encoding
        GCN::InstructionFormat eEncoding = pDecoder->ReadInstructionFormat( pBytes + nOffs );
        if( eEncoding == GCN::IF_UNKNOWN )
            throw gcnew System::Exception("Malformed GCN program");
            
        // check instruction length
        size_t nLength = pDecoder->DetermineInstructionLength(pLocation, eEncoding );
        if( nLength == 0 || (nLength%4) != 0 )
            throw gcnew System::Exception("Malformed GCN program");
            
        // check for overrun of instruction buffer
        nOffs += nLength;
        if( nOffs > nBytes )
            throw gcnew System::Exception("Malformed GCN program");

        // decode instruction and see if it's a scalar
        GCN::Instruction inst;
        pDecoder->Decode( &inst, pLocation, eEncoding);

        if( inst.GetClass() == GCN::IC_SCALAR )
        {
            GCN::ScalarInstruction* pI = (GCN::ScalarInstruction*)&inst;
            if( pI->GetBranchTarget() )
            {
                if( pI->IsConditionalJump() )
                {
                    Branches.push_back( pLocation );
                    pmManagedInstructions->Add( gcnew GCNBranch(inst) );
                }
                else if( pI->IsUnconditionalJump() )
                { 
                    Jumps.push_back( pLocation );
                    pmManagedInstructions->Add( gcnew GCNJump(inst) );
                }
            }
            else
            {
                pmManagedInstructions->Add( gcnew GCNInstruction(inst) );
            }
        }
        else
        {
            switch( inst.GetClass() )
            {
            case GCN::IC_IMAGE:
                {
                    GCN::ImageInstruction* pInst = static_cast<GCN::ImageInstruction*>( &inst );
                    if( pInst->IsFilteredFetch() )
                        pmManagedInstructions->Add( gcnew GCNSamplingOp( *pInst ) );
                    else if( pInst->IsUnfilteredLoadStore() )
                        pmManagedInstructions->Add( gcnew GCNTBufferOp( *pInst ) );
                    else
                        pmManagedInstructions->Add( gcnew GCNInstruction( *pInst ) );
                }
                break;
            case GCN::IC_BUFFER:
                {
                    GCN::BufferInstruction* pBuff = static_cast<GCN::BufferInstruction*>(&inst);
                        
                    // TBuffer means the instruction tells us the format
                    // non-TBuffer means the descriptor tells us, and that is the one where
                    // we need to create the special 'TexelLoad' subclass
                    if( !pBuff->IsTBuffer() )
                        pmManagedInstructions->Add( gcnew GCNTBufferOp( *pBuff ) );
                    else
                        pmManagedInstructions->Add( gcnew GCNInstruction( *pBuff ) );                        
                }
                break;
            default:
                pmManagedInstructions->Add( gcnew GCNInstruction(inst) );
            }
        }

        IndexMap.insert( {pLocation, IndexMap.size()} );
    }

        
    // Assign branch target instruction refs
    for( size_t i=0; i<Branches.size(); i++ )
    {
        const unsigned char* pLocation = Branches[i];
        GCN::InstructionFormat eEncoding = pDecoder->ReadInstructionFormat( pLocation );
        size_t nLength = pDecoder->DetermineInstructionLength(pLocation, eEncoding );
            
        GCN::ScalarInstruction inst;
        pDecoder->Decode( &inst, pLocation, eEncoding );

        IInstruction^ ifTarget   = pmManagedInstructions[ IndexMap[inst.GetBranchTarget()] ];
        IInstruction^ elseTarget = pmManagedInstructions[ IndexMap[pLocation+nLength] ];
        GCNBranch^ branch = dynamic_cast<GCNBranch^>( pmManagedInstructions[ IndexMap[pLocation] ] );
        branch->m_IfTarget = ifTarget;
        branch->m_ElseTarget = elseTarget;
    }

    // assign jump instruction refs
    for( size_t i=0; i<Jumps.size(); i++ )
    {
        const unsigned char* pLocation = Jumps[i];
        GCN::InstructionFormat eEncoding = pDecoder->ReadInstructionFormat( pLocation );
        size_t nLength = pDecoder->DetermineInstructionLength(pLocation, eEncoding );
            
        GCN::ScalarInstruction inst;
        pDecoder->Decode( &inst, pLocation, eEncoding );

        IInstruction^ target   = pmManagedInstructions[ IndexMap[inst.GetBranchTarget()] ];
        GCNJump^ jump = dynamic_cast<GCNJump^>( pmManagedInstructions[ IndexMap[pLocation] ] );
        jump->m_Target = target;
    }

    return pmManagedInstructions;
}

        
System::String^ Scrutinizer_GCN::AnalyzeExecutionTrace( List<IInstruction^>^ ops )
{
    std::vector<GCN::Simulator::SimOp> pOps(ops->Count);

    for( size_t i=0; i<ops->Count; i++ )
    {
        GCNInstruction^ gcnOp = static_cast<GCNInstruction^>( ops[i] );
        pOps[i].pInstruction = gcnOp->m_pInstruction;
        pOps[i].eFilter = GCN::Simulator::FILT_POINT;
        pOps[i].eFormat = GCN::Simulator::FMT_RGBA8;

        GCNSamplingOp^ sampling = dynamic_cast<GCNSamplingOp^>( gcnOp );
        if( sampling != nullptr )
        {
            // map Scrutinizer's enums onto the unmanaged simulator ones
            switch( sampling->Filter )
            {
            case TextureFilter::POINT:      pOps[i].eFilter = GCN::Simulator::FILT_POINT;      break;
            case TextureFilter::BILINEAR:   pOps[i].eFilter = GCN::Simulator::FILT_BILINEAR;   break;
            case TextureFilter::TRILINEAR:  pOps[i].eFilter = GCN::Simulator::FILT_TRILINEAR;  break;
            case TextureFilter::ANISO_2X:   pOps[i].eFilter = GCN::Simulator::FILT_ANISO_2X;   break;
            case TextureFilter::ANISO_4X:   pOps[i].eFilter = GCN::Simulator::FILT_ANISO_4X;   break;
            case TextureFilter::ANISO_8X:   pOps[i].eFilter = GCN::Simulator::FILT_ANISO_8X;   break;
            }

            switch( sampling->Format )
            {
            case TexelFormat::R8     :  pOps[i].eFormat = GCN::Simulator::FMT_R8       ; break;
            case TexelFormat::RG8    :  pOps[i].eFormat = GCN::Simulator::FMT_RG8      ; break;
            case TexelFormat::RGBA8  :  pOps[i].eFormat = GCN::Simulator::FMT_RGBA8    ; break;
            case TexelFormat::R16    :  pOps[i].eFormat = GCN::Simulator::FMT_R16      ; break;
            case TexelFormat::RG16   :  pOps[i].eFormat = GCN::Simulator::FMT_RG16     ; break;
            case TexelFormat::RGBA16 :  pOps[i].eFormat = GCN::Simulator::FMT_RGBA16   ; break;
            case TexelFormat::R16F   :  pOps[i].eFormat = GCN::Simulator::FMT_R16F     ; break;
            case TexelFormat::RG16F  :  pOps[i].eFormat = GCN::Simulator::FMT_RG16F    ; break;
            case TexelFormat::RGBA16F:  pOps[i].eFormat = GCN::Simulator::FMT_RGBA16F  ; break;
            case TexelFormat::R32F   :  pOps[i].eFormat = GCN::Simulator::FMT_R32F     ; break;
            case TexelFormat::RG32F  :  pOps[i].eFormat = GCN::Simulator::FMT_RG32F    ; break;
            case TexelFormat::RGBA32F:  pOps[i].eFormat = GCN::Simulator::FMT_RGBA32F  ; break;
            case TexelFormat::BC1    :  pOps[i].eFormat = GCN::Simulator::FMT_BC1      ; break;
            case TexelFormat::BC2    :  pOps[i].eFormat = GCN::Simulator::FMT_BC2      ; break;
            case TexelFormat::BC3    :  pOps[i].eFormat = GCN::Simulator::FMT_BC3      ; break;
            case TexelFormat::BC4    :  pOps[i].eFormat = GCN::Simulator::FMT_BC4      ; break;
            case TexelFormat::BC5    :  pOps[i].eFormat = GCN::Simulator::FMT_BC5      ; break;
            case TexelFormat::BC6    :  pOps[i].eFormat = GCN::Simulator::FMT_BC6      ; break;
            case TexelFormat::BC7    :  pOps[i].eFormat = GCN::Simulator::FMT_BC7      ; break;
            }
        }
    }

    GCN::Simulator::Settings settings;
    settings.nExportCost = 256;      // Based on Layla's "packman" slide.  1 export -> 64 ALU            // TODO: Sane number
    settings.nWaveIssueRate = 10;    // Assumes 1 wave/rasterizer/clk, round-robined among 10 CUs/slice   TODO: Sane number.
    settings.nWavesToExecute = 500;
    settings.nMaxWavesPerSIMD = m_pmShader->GetOccupancy();

    GCN::Simulator::Results results;
    GCN::Simulator::Simulate( results, settings, pOps.data(), pOps.size() );

    float fClocks = results.nCycles;

    float fStallRate =
        (results.nStallCycles[0]+results.nStallCycles[1]+results.nStallCycles[2]+results.nStallCycles[3])/fClocks;
    float fVALUUtil =
        (results.nVALUBusy[0]+results.nVALUBusy[1]+results.nVALUBusy[2]+results.nVALUBusy[3])/(4*fClocks);

    float fVMemUtil = (results.nVMemBusy)/fClocks;

    char buffer[4096];
    sprintf( buffer,
            "Clocks: %u\n (%.2f clocks/wave)\n"
            "VALU util: %.2f%%\n"
            "VMem util: %.2f%%\n"
            "Stall rate: %.2f%%\n",
            results.nCycles,
            fClocks / settings.nWavesToExecute,
            100.0f*fVALUUtil,
            100.0f*fVMemUtil,
            100.0f*fStallRate
        );

    System::String^ str = gcnew System::String(buffer);
    return str->Replace("\n", System::Environment::NewLine );
}

