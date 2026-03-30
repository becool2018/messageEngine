  ---                                                                                                                                                                     
  name: read-standards
  description: Read and internalize project coding standards from CLAUDE.md and .claude/CLAUDE.md. Use when starting any task that involves writing or reviewing C/C++
  code.                                                                                                                                                                   
  user-invocable: true
  allowed-tools: Read                                                                                                                                                     
  ---                                                                                                                                                                     
   
  Read both project standards files in full before proceeding:                                                                                                            
                  
  1. Read `CLAUDE.md` — application-specific requirements (messaging, transport, impairment engine, safety, traceability)                                                 
  2. Read `.claude/CLAUDE.md` — global C/C++ standards (Power of 10, MISRA C++:2023, F´ subset, architecture rules)
                                                                                                                                                                          
  After reading both, confirm you have internalized:                                                                                                                      
  - Power of 10 rules (especially no recursion, fixed loop bounds, no dynamic alloc after init, assertion density)                                                        
  - MISRA C++:2023 requirements                                                                                                                                           
  - Layering rules (src/core → src/platform, no reverse deps)
  - Safety-critical function annotation requirements                                                                                                                      
  - Traceability policy (Implements/Verifies tags)
                                                                                                                                                                          
  Then proceed with the user's original request, citing relevant rules when design decisions are shaped by them
