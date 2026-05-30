# nl-router

[![build](../../actions/workflows/build.yml/badge.svg?branch=main)](../../actions/workflows/build.yml)

A DICOM router loosely inspired by [mercure](https://mercure-imaging.org/) and
[dicomdiablo](https://github.com/mercure-imaging/mercure) — re-architected for
high-throughput receive, centralized configuration, and a multi-node deployment
model.

## Status

Pre-alpha. Currently scaffolding — only the database schema is implemented.
See `plan.md` (or the design plan referenced in the project root) for the full
architecture and module map.

## Architecture (one-page summary)

```
                                                                       
   DICOM modality   ──►   Receiver (C++, DCMTK)                        
                              │  writes files to local disk            
                              ▼  inserts work_queue row                
                         ┌────────────┐                                
                         │ work_queue │  Postgres (central)            
                         └─────┬──────┘                                
                               │ status='received'                     
                               ▼                                       
                          Router (C++)                                 
                          - PEGTL DSL parser                           
                          - evaluates rules                            
                          - writes route_assignments                   
                               │ status='routed'                       
                               ▼                                       
                         Processor workers (any language)              
                          - long-running, pull from processing_jobs    
                          - built-in modules ship as native binaries   
                          - custom modules can be Docker containers    
                               │ status='processed'                    
                               ▼                                       
                          Dispatcher (C++)                             
                          - per-destination thread pools               
                          - dicom / dicomweb / gcp_dicomweb / http /   
                            file / object_storage destinations         
                               │ status='dispatched' or _partial       
                               ▼                                       
                          Cleaner (C++)                                
                          - deletes files after configured TTL         
                          - prunes DB rows on a longer TTL             
                                                                       
                                                                       
   Management plane (Python):  FastAPI + Jinja2 + HTMX UI + Typer CLI  
                                                                       
```

## Modules

| Process | Language | Purpose |
|---|---|---|
| `nl-receiver` | C++ / DCMTK | DICOM SCP (C-ECHO, C-STORE, C-FIND, C-MOVE, MWL); 100 concurrent associations |
| `nl-route` | C++ / PEGTL | Evaluates rule predicates, writes route_assignments |
| `nl-dispatch` | C++ | Sends to destinations; per-destination thread pool |
| `nl-clean` | C++ | TTL-based file cleanup + leader-elected row pruning |
| `nl-mod-*` | C++ (built-in) / any (custom) | Processing module workers |
| `nl-router` (Python) | Python / FastAPI | API + UI + CLI |

All modules talk to one central Postgres for config, queue, and audit.

## Quick start (once implementation lands)

```bash
# Bring up the dev stack
docker-compose up -d

# Apply migrations
make migrate

# Initialize (creates admin user + initial API token)
nl-router init

# Start the API
systemctl start nl-router-api   # or: docker-compose up api
```

## Layout

```
migrations/        SQL migrations (golang-migrate format)
cpp/               C++ services and processing modules
  common/          shared library
  receiver/        DICOM SCP
  router/          rule evaluator
  dispatcher/      outbound sender
  cleaner/         file/row cleaner
  modules/         built-in processing modules
  getdcmtags/      tag extraction helper
python/            FastAPI API + Jinja2 UI + Typer CLI
packaging/         nfpm configs, systemd units, install.sh
docs/              operator and developer documentation
dashboards/        shipped Grafana dashboard JSON
```

## Requirements

- Linux (x86_64 or arm64)
- Postgres 14+ (16+ recommended) — external, not bundled
- PgBouncer recommended for production
- Python 3.11+ on nodes running the management API
- DCMTK 3.6+ build dependencies on nodes running DICOM hot-path binaries

## License

TBD.
