#include <iostream>
#include <cmath>
#include "search.h"
#include "eval.h"
#include "misc.h"
#include "movescore.h"
#include "see.h"

int LMRTable[MAXDEPTH][256];

void InitSearch()
{
    for (int depth = 1; depth < MAXDEPTH; depth++)
    {
        for (int played = 1; played < 256; played++)
        {
            LMRTable[depth][played] = 1 + log(depth) * log(played) / 2;
        }
    }
}

static void CheckUp(SearchInfo &info)
{
    if (info.timeset == true && GetTimeMs() > info.end_time)
    {
        info.stopped = true;
    }
}

void ClearForSearch(SearchInfo &info, TranspositionTable *table)
{
    info.nodes = 0;
    info.stopped = false;

    // Reset history
    for (int x = 0; x < 12; x++)
    {
        for (int i = 0; i < 64; i++)
        {
            info.searchHistory[x][i] = 0;
            // info.searchCounters[i][i] = NO_MOVE;
        }
    }

    table->currentAge++;
}

/* Quiescence Search to prevent Horizon Effect.*/
int Quiescence(int alpha, int beta, Board &board, SearchInfo &info, SearchStack *ss)
{
    /* Checking for time every 2048 nodes */
    if ((info.nodes & 2047) == 0)
    {
        CheckUp(info);
    }

    /* We return static evaluation if we exceed max depth */
    if (info.ply > MAXPLY - 1)
    {
        return Evaluate(board, info.pawnTable);
    }

    /* Repetition check */
    if (board.isRepetition())
    {
        return 0;
    }

    int score = Evaluate(board, info.pawnTable);
    if (score >= beta)
    {
        return beta;
    }
    if (score > alpha)
    {
        alpha = score;
    }

    /* Move generation */
    /* Generate capture moves for current position*/

    int bestscore = score;
    score = -INF_BOUND;

    Movelist list;
    Movegen::legalmoves<CAPTURE>(board, list);

    score_moves(board, &list);

    /* Move loop */
    for (int i = 0; i < list.size; i++)
    {
        pickNextMove(i, &list);

        Move move = list.list[i].move;

        // Make the move on board
        board.makeMove(move);
        // Increment ply and nodes
        info.ply++;
        info.nodes++;

        // Call Quiescence on current position
        ss->move = move;
        score = -Quiescence(-beta, -alpha, board, info, ss + 1);
        // Undo move on board
        board.unmakeMove(move);

        // Decrement ply
        info.ply--;

        if (info.stopped)
        {
            return 0;
        }

        if (score > bestscore)
        {
            bestscore = score;
            if (score > alpha)
            {
                alpha = score;
                if (score >= beta)
                {
                    break;
                }
            }
        }
    }
    return bestscore;
}

int AlphaBeta(int alpha, int beta, int depth, Board &board, SearchInfo &info, SearchStack *ss, TranspositionTable *table)
{

    // init pv length
    info.pv_table.length[info.ply] = info.ply;

    if (depth <= 0)
    {
        return Quiescence(alpha, beta, board, info, ss);
    }

    /* Checking for time every 2048 nodes */
    if ((info.nodes & 2047) == 0)
    {
        CheckUp(info);
    }

    bool isRoot = (info.ply == 0);
    bool inCheck = board.isSquareAttacked(~board.sideToMove, board.KingSQ(board.sideToMove));
    bool isPvNode = (beta - alpha) > 1;
    int score = -INF_BOUND;
    int eval = 0;
    // bool improving = false;

    /* We return static evaluation if we exceed max depth */
    if (info.ply > MAXPLY - 1)
    {
        return Evaluate(board, info.pawnTable);
    }

    /* Repetition & Fifty move check */
    if ((board.isRepetition()) && info.ply)
    {
        return 0;
    }

    TTEntry tte;
    bool ttHit = ss->excluded != NO_MOVE ? false : table->probeEntry(board.hashKey, &tte, info.ply);

    // If we hit a transposition table entry and the depth is equal to or greater and the following conditions meet
    // we will return the tt score.
    if (!isPvNode && ttHit && tte.depth >= depth)
    {
        if ((tte.flag == HFALPHA && tte.score <= alpha) || (tte.flag == HFBETA && tte.score >= beta) || (tte.flag == HFEXACT))
            return tte.score;
    }

    ss->static_eval = eval = ttHit ? tte.eval : Evaluate(board, info.pawnTable);
    // improving = !inCheck && ss->static_eval > (ss - 2)->static_eval;

    /* In check extension */
    if (inCheck)
    {
        ss->static_eval = eval = 0;
        depth++;
    }

    if (!isPvNode && !inCheck && info.ply)
    {

        // we can use tt score
        if (ttHit)
        {
            eval = tte.score;
        }

        // Reverse Futility Pruning (RFP)
        // If the eval is well above beta by a margin, then we assume the eval will hold above beta.
        if (depth <= 5 && eval >= beta && eval - (depth * 75) >= beta)
        {
            return eval;
        }

        // Null move pruning
        if (eval >= beta && ss->static_eval >= beta && board.nonPawnMat(board.sideToMove) && (depth >= 3) && ((ss - 1)->move != NULL_MOVE))
        {
            int R = 3 + depth / 3 + std::min((eval - beta) / 200, 3);
            board.makeNullMove();
            ss->move = NULL_MOVE;
            info.ply++;

            score = -AlphaBeta(-beta, -beta + 1, depth - R, board, info, ss + 1, table);

            info.ply--;
            board.unmakeNullMove();

            if (info.stopped)
            {
                return 0;
            }

            if (score >= beta)
            {
                // dont return mate scores
                if (score > ISMATE)
                    score = beta;

                return score;
            }
        }
    }

    /* Initialize variables */
    int bestscore = -INF_BOUND;
    int moveCount = 0;
    int quietsSearched = 0;
    int oldAlpha = alpha;
    Move bestmove = NO_MOVE;

    // Reset score
    score = -INF_BOUND;

    /* Move generation */
    /* Generate all legal moves for current position */
    Movelist list;
    Movegen::legalmoves<ALL>(board, list);

    /* Score moves and pass the tt move so it can be sorted highest */
    score_moves(board, &list, ss, &info, tte.move);

    /* Move loop */
    for (int i = 0; i < list.size; i++)
    {
        // Pick move with highest possible score
        pickNextMove(i, &list);

        // Initialize move variable
        Move move = list.list[i].move;

        if (move == ss->excluded)
            continue;

        bool isQuiet = (!promoted(move) && !is_capture(board, move));
        int extension = 0;

        // if (!isRoot)
        // {
        //     if ((ss->excluded == NO_MOVE) && depth >= 10 && (move == tte.move) && (tte.flag == HFBETA) && abs(tte.score) < ISMATE && tte.depth >= depth - 2)
        //     {
        //         int singularBeta = tte.score - 2 * depth;
        //         int singularDepth = (depth - 1) / 2;

        //         ss->excluded = tte.move;
        //         int singularScore = AlphaBeta(singularBeta - 1, singularBeta, singularDepth, board, info, ss, table);
        //         ss->excluded = NO_MOVE;

        //         if (singularScore < singularBeta)
        //         {
        //             extension = 1;
        //         }
        //     }
        // }

        // if (!isRoot && bestscore > -ISMATE)
        //{

        if (!isRoot && bestscore > -ISMATE)
        {

            // SEE pruning
            if (isQuiet)
            {
                // Late Move Pruning/Movecount pruning
                if (!isPvNode && !inCheck && depth < 4 && (quietsSearched >= depth * depth * 4))
                {
                    continue;
                }

                // SEE pruning for quiets
                if (depth < 6 && !see(board, move, -50 * depth))
                {
                    continue;
                }
            }
            else
            {

                // SEE pruning for non quiets
                if (depth < 4 && !see(board, move, -45 * depth))
                {
                    continue;
                }
            }
        }

        int newDepth = depth + extension;

        // Make the move on board
        board.makeMove(move);

        ss->move = move;
        // Increment ply and nodes
        info.ply++;
        info.nodes++;
        moveCount++;

        if (isQuiet)
        {
            quietsSearched++;
        }

        bool do_fullsearch = false;

        // Late move reduction
        if (!inCheck && depth >= 3 && moveCount > (2 + 2 * isPvNode) && isQuiet)
        {
            int reduction = LMRTable[std::min(depth, 63)][moveCount];


            reduction = std::min(depth - 1, std::max(1, reduction));

            score = -AlphaBeta(-alpha - 1, -alpha, newDepth - reduction, board, info, ss + 1, table);

            do_fullsearch = score > alpha && reduction != 1;
        }
        else
        {
            do_fullsearch = !isPvNode || moveCount > 1;
        }

        // Full depth search on a null window
        if (do_fullsearch)
        {
            score = -AlphaBeta(-alpha - 1, -alpha, newDepth - 1, board, info, ss + 1, table);
        }

        // Principal Variation Search (PVS)
        if (isPvNode && (moveCount == 1 || (score > alpha && score < beta)))
        {
            score = -AlphaBeta(-beta, -alpha, newDepth - 1, board, info, ss + 1, table);
        }

        // Undo move on board
        board.unmakeMove(move);
        // Decrement ply
        info.ply--;

        if (info.stopped == true && !isRoot)
        {
            return 0;
        }

        if (score > bestscore)
        {
            bestscore = score;

            if (score > alpha)
            {
                // Record PV
                alpha = score;

                bestmove = move;

                info.pv_table.array[info.ply][info.ply] = bestmove;
                for (int next_ply = info.ply + 1; next_ply < info.pv_table.length[info.ply + 1]; next_ply++)
                {
                    info.pv_table.array[info.ply][next_ply] = info.pv_table.array[info.ply + 1][next_ply];
                }

                info.pv_table.length[info.ply] = info.pv_table.length[info.ply + 1];

                if (score >= beta)
                {
                    // Update killers
                    if (isQuiet)
                    {
                        // Move previous_move = (ss - 1)->move;

                        ss->killers[1] = ss->killers[0];
                        ss->killers[0] = move;

                        // info.searchCounters[from(previous_move)][to(previous_move)] = move;
                    }

                    break;
                }
                if (isQuiet)
                {
                    info.searchHistory[board.pieceAtB(from(move))][to(move)] += depth;
                }
            }
        }

        // We are safe to break out of the loop if we are sure that we don't return illegal move
        if (info.stopped == true && isRoot && info.pv_table.array[0][0] != NO_MOVE)
        {
            break;
        }
    }

    if (moveCount == 0)
    {
        if (inCheck)
        {
            return -ISMATE + info.ply; // Checkmate
        }
        else
        {
            return 0; // Stalemate
        }
    }

    int flag = bestscore >= beta ? HFBETA : (alpha != oldAlpha) ? HFEXACT
                                                                : HFALPHA;

    if (ss->excluded == NO_MOVE)
    {
        table->storeEntry(board.hashKey, flag, bestmove, depth, bestscore, ss->static_eval, info.ply);
    }

    return bestscore;
}

void SearchPosition(Board &board, SearchInfo &info, TranspositionTable *table)
{
    ClearForSearch(info, table);

    // Initialize search stack

    SearchStack stack[MAXPLY + 10];
    SearchStack *ss = stack + 7; // Have some safety overhead.

    int score = 0;

    long startime = GetTimeMs();
    Move bestmove = NO_MOVE;

    for (int current_depth = 1; current_depth <= info.depth; current_depth++)
    {
        score = AlphaBeta(-INF_BOUND, INF_BOUND, current_depth, board, info, ss, table);
        if (info.stopped == true)
        {
            break;
        }
        bestmove = info.pv_table.array[0][0];
        std::cout << "info score cp " << score << " depth " << current_depth << " nodes " << info.nodes << " time " << (GetTimeMs() - startime) << " pv";

        for (int i = 0; i < info.pv_table.length[0]; i++)
        {
            std::cout << " " << convertMoveToUci(info.pv_table.array[0][i]);
        }
        std::cout << "\n";
    }

    std::cout << "bestmove " << convertMoveToUci(bestmove) << "\n";
}